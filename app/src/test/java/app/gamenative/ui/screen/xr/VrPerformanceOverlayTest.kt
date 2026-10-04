package app.gamenative.ui.screen.xr

import android.graphics.Bitmap
import android.graphics.Color
import java.io.File
import org.junit.Assert.*
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config
import org.robolectric.annotation.GraphicsMode

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [33])
@GraphicsMode(GraphicsMode.Mode.NATIVE)
class VrPerformanceOverlayTest {
    @Test fun graphsAndConditionalStatsRenderLegibly() {
        val renderer = VrPerformanceOverlay(0)
        var bitmap: Bitmap? = null
        for (i in 1..60) {
            bitmap = renderer.draw(doubleArrayOf(1.0, i.toDouble(), 72.0,
                if(i%9==0) 64.2 else 71.8, if(i%7==0) 36.2 else 48.5,
                0.8, 1.2, 6.4, 31.0, 1678.0,1846.0,1964.0,2160.0,32.5), 87)
        }
        val image = bitmap!!
        assertEquals(1024, image.width); assertEquals(256, image.height)
        val background = image.getPixel(0, 0)
        fun changed(left: Int, top: Int, right: Int, bottom: Int): Int {
            var count = 0
            for(y in top until bottom) for(x in left until right)
                if(image.getPixel(x,y)!=background) count++
            return count
        }
        assertTrue(changed(768,80,1024,240)>500) // All three timing rows.
        assertTrue(changed(104,80,392,216)>500) // Present graph.
        assertTrue(changed(428,80,716,216)>500) // Game graph.
        assertEquals(155, Color.alpha(image.getPixel(0,0)))
        val out=File(System.getProperty("java.io.tmpdir"),"gamenative-vr-performance-preview.png")
        out.outputStream().use { image.compress(Bitmap.CompressFormat.PNG,100,it) }
        renderer.draw(doubleArrayOf(1.0,61.0,72.0,71.8,48.5,18.9,Double.NaN,Double.NaN,25.0,
            2014.0,2216.0,1964.0,2160.0),null)
        File(System.getProperty("java.io.tmpdir"),"gamenative-vr-performance-bypass.png").outputStream().use {
            image.compress(Bitmap.CompressFormat.PNG,100,it)
        }
        renderer.draw(doubleArrayOf(1.0,62.0,72.0,71.8,48.5,Double.NaN,Double.NaN,Double.NaN,0.0,0.0,0.0,0.0,0.0),null)
        assertTrue(changed(768,105,1024,130)>100) // PRESENT remains with effects off.
        assertEquals(0,changed(768,132,1024,240)) // No stale filter/FG labels.
    }
}
