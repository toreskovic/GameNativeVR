package app.gamenative.gamefixes

import android.content.Context
import app.gamenative.data.GameSource
import com.winlator.container.Container
import java.io.File
import timber.log.Timber

// Launch the VR game directly instead of the SHVR launcher selected by default.
val STEAM_Fix_617830: KeyedGameFix = object : KeyedGameFix {
    override val gameSource = GameSource.STEAM
    override val gameId = "617830"

    override fun apply(
        context: Context,
        gameId: String,
        installPath: String,
        installPathWindows: String,
        container: Container,
    ): Boolean = try {
        val selected = container.executablePath
        val filename = selected.replace('\\', '/').substringAfterLast('/')
        if (selected.isBlank() || filename.equals("SHVR.exe", ignoreCase = true)) {
            val executable = File(installPath).listFiles()?.firstOrNull {
                it.isFile && it.name.equals("SUPERHOT_VR.exe", ignoreCase = true)
            }
            if (executable != null) {
                container.executablePath = executable.name
                container.saveData()
                Timber.tag("GameFixes").i("Selected SUPERHOT_VR.exe for SUPERHOT VR")
            } else {
                Timber.tag("GameFixes").w("SUPERHOT_VR.exe is missing; keeping the current executable")
            }
        }
        true
    } catch (e: Exception) {
        Timber.tag("GameFixes").e(e, "Failed to update SUPERHOT VR executable")
        false
    }
}
