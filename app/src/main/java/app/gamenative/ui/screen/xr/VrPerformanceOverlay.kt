package app.gamenative.ui.screen.xr

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.PorterDuff
import android.graphics.Paint
import android.graphics.Path
import android.graphics.Typeface
import app.gamenative.powercontrol.metrics.GpuUsageSampler
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import java.util.Locale
import kotlin.math.max
import kotlin.math.roundToInt

/** Drawn on a worker, uploaded only on a new one-second sample. Native XR owns
 * the head-relative quad and controller chord; this never captures game pixels. */
internal class VrPerformanceOverlay(private val handle: Long) {
    private val bitmap = Bitmap.createBitmap(1024, 256, Bitmap.Config.ARGB_8888)
    private val canvas = Canvas(bitmap)
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Typeface.create("sans-serif-condensed", Typeface.BOLD)
    }
    private val gpu = GpuUsageSampler()
    private val present = ArrayDeque<Float>()
    private val game = ArrayDeque<Float>()
    private val white = Color.rgb(239, 245, 252)
    private val muted = Color.rgb(150, 171, 193)
    private val green = Color.rgb(101, 242, 144)
    private val blue = Color.rgb(99, 201, 255)

    suspend fun run() {
        var wasVisible = false
        var sequence = -1.0
        try {
            while (currentCoroutineContext().isActive) {
                val data = XrNative.nativePerformanceSnapshot(handle) ?: break
                val visible = data[0] != 0.0
                if (visible && (!wasVisible || data[1] != sequence)) {
                    if (!wasVisible || data[1] < sequence) {
                        present.clear(); game.clear(); gpu.reset()
                    }
                    draw(data, gpu.sample()?.percent)
                    XrNative.nativeSubmitPerformanceBitmap(handle, bitmap)
                    sequence = data[1]
                }
                wasVisible = visible
                delay(100) // Visibility only; metrics, sysfs reads and drawing are one-second work.
            }
        } finally {
            bitmap.recycle()
        }
    }

    private fun append(history: ArrayDeque<Float>, value: Double) {
        if (history.size == 60) history.removeFirst()
        history.addLast(value.toFloat())
    }

    private fun text(value: String, x: Float, y: Float, size: Float, color: Int = white) {
        paint.style = Paint.Style.FILL; paint.color = color; paint.textSize = size
        canvas.drawText(value, x, y, paint)
    }
    private fun number(value: Double): String =
        if (value.isFinite()) String.format(Locale.ROOT, "%.1f", value) else "--"

    internal fun draw(data: DoubleArray, gpuPercent: Int?): Bitmap {
        if(data[1]>0) { append(present,data[3]); append(game,data[4]) }
        canvas.drawColor(Color.argb(155, 10, 15, 23), PorterDuff.Mode.SRC)
        val hz = data[2].takeIf { it.isFinite() && it > 0 }?.toFloat() ?: 0f
        text(if (hz > 0) "${hz.roundToInt()}" else "--", 14f, 100f, 36f)
        text("Hz", 18f, 129f, 24f, muted)
        graph("OPENXR", data[3], present, 104f, hz, green)
        graph("GAME", data[4], game, 428f, hz, blue)
        text("GPU", 768f, 51f, 30f, muted)
        text(gpuPercent?.let { "$it%" } ?: "N/A", 930f, 51f, 32f)
        var y = 86f
        val flags = data[8].toInt()
        text("REPEAT", 768f, y, 24f, muted)
        text("${number(data.getOrElse(13) { Double.NaN })}%", 930f, y, 25f)
        y += 36f
        text("PRESENT", 768f, y, 23f, muted)
        text("${number(data[5])} ms", 930f, y, 23f)
        val effects = buildList {
            if(flags and 1 != 0) add("AA")
            if(flags and 2 != 0) add("SGSR")
        }.joinToString(" + ")
        if(effects.isNotEmpty()) text("+ $effects", 768f, y+21f, 19f, muted)
        if(flags and 16 != 0 && flags and 2 == 0)
            text("SGSR bypass", 768f, y+43f, 17f, muted)
        if(flags and 4 != 0) {
            text("FG", 768f, 198f, 23f, muted)
            text("${number(data[7])} ms", 930f, 198f, 23f)
        }
        fun resolution(index: Int): String = if(data[index]>0 && data[index+1]>0)
            "${data[index].toInt()}×${data[index+1].toInt()}" else "--"
        text("IN ${resolution(9)} → OUT ${resolution(11)} /eye", 104f, 213f, 22f, muted)
        text("1 s/sample · PRESENT includes active filters · times for both eyes", 104f, 243f, 20f, muted)
        return bitmap
    }

    private fun graph(label: String, value: Double, history: ArrayDeque<Float>, x: Float, hz: Float, color: Int) {
        val width = 288f; val top = 80f; val bottom = 181f
        text(label, x, 33f, 24f, muted)
        text("${number(value)} FPS", x, 65f, 32f, color)
        val ceiling = max(max(hz, 1f), history.filter { it.isFinite() }.maxOrNull() ?: 0f)
        paint.strokeWidth = 1.5f; paint.color = Color.rgb(46, 60, 76)
        for (i in 0..2) {
            val y = top + (bottom - top) * i / 2
            canvas.drawLine(x, y, x + width, y, paint)
        }
        paint.color = muted; paint.strokeWidth = 2f
        canvas.drawLine(x, top, x, bottom, paint)
        canvas.drawLine(x, bottom, x + width, bottom, paint)
        text("0", x - 15, bottom + 6, 18f, muted)
        val path = Path()
        var started = false
        history.forEachIndexed { i, fps ->
            if (fps.isFinite()) {
                val px = x + width * (60 - history.size + i) / 59f
                val py = bottom - (fps / ceiling).coerceIn(0f, 1f) * (bottom - top)
                if (started) path.lineTo(px, py) else { path.moveTo(px, py); started = true }
                if (history.size == 1) { paint.color = color; canvas.drawCircle(px, py, 3f, paint) }
            } else started = false
        }
        paint.color = color; paint.style = Paint.Style.STROKE; paint.strokeWidth = 3f
        canvas.drawPath(path, paint)
        paint.style = Paint.Style.FILL
    }
}
