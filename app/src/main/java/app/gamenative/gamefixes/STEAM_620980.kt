package app.gamenative.gamefixes

import android.content.Context
import app.gamenative.data.GameSource
import com.winlator.container.Container
import timber.log.Timber

// Beat Saber's flat-screen argument prevents the VR loading sequence from completing.
val STEAM_Fix_620980: KeyedGameFix = object : KeyedGameFix {
    override val gameSource = GameSource.STEAM
    override val gameId = "620980"

    override fun apply(
        context: Context,
        gameId: String,
        installPath: String,
        installPathWindows: String,
        container: Container,
    ): Boolean = try {
        val args = beatSaberVrLaunchArgs(container.execArgs)
        if (args != container.execArgs) {
            container.execArgs = args
            container.saveData()
            Timber.tag("GameFixes").i("Removed Beat Saber's incompatible -fpfc launch argument")
        }
        true
    } catch (e: Exception) {
        Timber.tag("GameFixes").e(e, "Failed to update Beat Saber launch arguments")
        false
    }
}

internal fun beatSaberVrLaunchArgs(arguments: String): String {
    // Keep quoted arguments intact, including other arguments containing the text -fpfc.
    val tokens = Regex("""(?:[^\s"']+|"[^"]*"|'[^']*')+""").findAll(arguments).toList()
    val retained = tokens.filterNot { it.value in setOf("-fpfc", "\"-fpfc\"", "'-fpfc'") }
    return if (retained.size == tokens.size) arguments else retained.joinToString(" ") { it.value }
}
