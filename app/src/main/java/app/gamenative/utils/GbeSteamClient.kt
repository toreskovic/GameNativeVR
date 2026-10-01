package app.gamenative.utils

import android.content.Context
import app.gamenative.mods.ModArchiveExtractor
import app.gamenative.service.SteamService
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.File
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.security.MessageDigest

/** Pinned upstream cold-client payload. No example settings or game files are installed. */
object GbeSteamClient {
    const val VERSION = "release-2026_09_27-vs22"
    const val URL = "https://github.com/Detanup01/gbe_fork/releases/download/release-2026_09_27/emu-win-release-vs22.7z"
    const val ARCHIVE_SHA256 = "300186c34f49a9ea75b33ce2af299267cc1dad0225cf45e81a229578d4e11c85"
    private const val MARKER = ".gamenative-gbe-version"
    private val mutex = Mutex()
    internal data class PayloadFile(val source: String, val target: String, val sha256: String)
    internal val files = listOf(
        PayloadFile("steamclient.dll", "steamclient.dll", "64ce34be1c3ec008b6f661f7dc8e17ba7963cfa38e9a6b763ddf2b48e7cc4345"),
        PayloadFile("steamclient64.dll", "steamclient64.dll", "319051c75609187f6d320b3df2d0da4eadbed72b4188144ff06fe6eff30a90d3"),
        PayloadFile("steamclient_loader_x64.exe", "steamclient_loader_x64.exe", "0c89b372ab57dd8c97480d22f2727d832daf59230472a1e5520c1b61c4371be1"),
        PayloadFile("steamclient_loader_x86.exe", "steamclient_loader_x32.exe", "093c6968a7f93e552af41b2a456032dc4d62ed8cb2398cbcbef2dad2ff3cfa63"),
        PayloadFile("GameOverlayRenderer.dll", "GameOverlayRenderer.dll", "1d5774dc5e77f504babee197fece9eb36d913575fd77a29f7dfdc589f8ca65ab"),
        PayloadFile("GameOverlayRenderer64.dll", "GameOverlayRenderer64.dll", "2348ed930ab3b1bdc924103a41659e7a2acd61e8960d33719144db34d5538a75"),
        PayloadFile("extra_dlls/steamclient_extra_x64.dll", "extra_dlls/steamclient_extra_x64.dll", "b2cea3ff6a524463733d15bc3ab14cc4988dd1d0c972d8c6093912fd8ac56ac0"),
        PayloadFile("extra_dlls/steamclient_extra_x86.dll", "extra_dlls/steamclient_extra_x86.dll", "13910b9cfb79f17399bf9a1baa88f09aebc7e63386301faffe69648b93f4f561"),
    )

    internal fun sha256(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        file.inputStream().buffered().use { input ->
            val buffer = ByteArray(65536)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                digest.update(buffer, 0, count)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }

    private fun matches(file: File, hash: String) = file.isFile && sha256(file) == hash
    private fun validSource(root: File) = files.all { matches(File(root, it.source), it.sha256) }

    fun isInstalled(root: File): Boolean =
        File(root, MARKER).takeIf { it.isFile }?.readText() == VERSION &&
            files.all { matches(File(root, it.target), it.sha256) }

    fun invalidate(root: File) { File(root, MARKER).delete() }

    suspend fun prepare(context: Context, onProgress: (Float) -> Unit = {}): File =
        withContext(Dispatchers.IO) {
            mutex.withLock {
                val cache = File(context.filesDir, "gbe/$VERSION").apply { mkdirs() }
                val extracted = File(cache, "extracted")
                val source = File(extracted, "release/steamclient_experimental")
                if (!validSource(source)) {
                    val archive = File(cache, "emu-win-release-vs22.7z")
                    if (!matches(archive, ARCHIVE_SHA256)) {
                        archive.delete()
                        SteamService.fetchFile(URL, archive, onProgress)
                        if (!matches(archive, ARCHIVE_SHA256)) {
                            archive.delete()
                            error("Steam compatibility download failed SHA-256 verification")
                        }
                    }
                    ModArchiveExtractor.extract(archive, extracted)
                    check(validSource(source)) { "Steam compatibility archive has missing or invalid files" }
                }
                source
            }
        }

    /** Validate and stage everything before replacing any installed file; roll back on failure. */
    internal fun installFiles(source: File, root: File, payload: List<PayloadFile> = files) {
        check(payload.all { matches(File(source, it.source), it.sha256) }) { "Invalid Steam compatibility payload" }
        root.mkdirs()
        val work = Files.createTempDirectory(root.toPath(), ".gbe-install-").toFile()
        val replaced = mutableListOf<Pair<File, File?>>()
        try {
            payload.forEachIndexed { index, entry ->
                File(source, entry.source).copyTo(File(work, "new-$index"))
            }
            payload.forEachIndexed { index, entry ->
                val target = File(root, entry.target)
                target.parentFile?.mkdirs()
                val backup = if (target.exists()) File(work, "old-$index").also { target.copyTo(it) } else null
                replaced += target to backup
                Files.move(File(work, "new-$index").toPath(), target.toPath(), StandardCopyOption.REPLACE_EXISTING)
            }
        } catch (failure: Exception) {
            replaced.asReversed().forEach { (target, backup) ->
                runCatching {
                    if (backup != null) backup.copyTo(target, overwrite = true) else target.delete()
                }.exceptionOrNull()?.let { failure.addSuppressed(it) }
            }
            throw failure
        } finally {
            work.deleteRecursively()
        }
    }

    fun install(source: File, root: File) {
        invalidate(root)
        installFiles(source, root)
        File(root, MARKER).writeText(VERSION)
    }
}
