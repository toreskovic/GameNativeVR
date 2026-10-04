package app.gamenative.ui.screen.xr.windows

import android.content.Context
import android.util.Log
import java.util.concurrent.TimeUnit
import com.winlator.container.Container
import com.winlator.core.envvars.EnvVars
import com.winlator.xenvironment.ImageFs
import org.json.JSONObject
import java.io.File

/** App-owned explicit layer: never changes the system Vulkan configuration. */
object WindowsVrFoveation {
    private const val LAYER = "VK_LAYER_GN_vr_foveation"

    fun configure(context: Context, container: Container, directory: File, env: EnvVars): String {
        // Remove only our previous activation, preserving user-specified layers.
        val layers = env.get("VK_INSTANCE_LAYERS").split(':').filter { it.isNotBlank() && it != LAYER }
        env.put("VK_INSTANCE_LAYERS", layers.joinToString(":"))
        env.remove("GN_VR_FFR_LIBRARY")
        env.put("GN_VR_FFR", "0")
        env.put("GN_VR_FFR_TILE_PREVIEW", if (container.xrFfrDebug == 1) "1" else "0")
        val mode = container.xrFoveation.coerceIn(0, 2)
        if (mode == 0) return "Off"
        val library = File(context.applicationInfo.nativeLibraryDir, "libVkLayer_GN_vr_foveation.so")
        check(library.isFile) { "VR foveation layer is missing from this APK" }
        val manifestDirectory = File(directory, "vulkan-layers").apply { check(exists() || mkdirs()) }
        val layer = JSONObject().put("name", LAYER).put("type", "GLOBAL")
            .put("library_path", library.path).put("api_version", "1.3.0")
            .put("implementation_version", "1").put("description", "GameNative experimental VR foveation")
        File(manifestDirectory, "gn_vr_foveation.json").writeText(
            JSONObject().put("file_format_version", "1.2.0").put("layer", layer).toString(),
        )
        val share = ImageFs.find(context).rootDir.resolve("usr/share/vulkan")
        val paths = listOf(manifestDirectory.path, env.get("VK_LAYER_PATH"),
            share.resolve("implicit_layer.d").path, share.resolve("explicit_layer.d").path)
            .filter(String::isNotBlank).distinct()
        env.put("VK_LAYER_PATH", paths.joinToString(":"))
        env.put("VK_INSTANCE_LAYERS", (layers + LAYER).joinToString(":"))
        env.put("GN_VR_FFR", mode)
        env.put("GN_VR_FFR_LIBRARY", library.path)
        // Tested compatibility override; explicit container value 0 restores strict checks.
        if (!env.has("GN_VR_FFR_ALLOW_MISSING_FORMAT_BIT")) {
            env.put("GN_VR_FFR_ALLOW_MISSING_FORMAT_BIT", "1")
        }
        if (env.get("GN_VR_FFR_PROBE") == "1") {
            startDriverProbe(context, directory, env)
        } else {
            File(directory, "ffr-driver-probe.log").delete()
        }
        val engine = detectEngine(container)
        env.put("GN_VR_FFR_ENGINE", engine)
        return "mode=$mode engine=$engine (activation and selections logged as GN-VR-FFR)"
    }

    private fun startDriverProbe(context: Context, directory: File, env: EnvVars) {
        val output = File(directory, "ffr-driver-probe.log")
        val driverPath = env.get("ADRENOTOOLS_DRIVER_PATH")
        val driverName = env.get("ADRENOTOOLS_DRIVER_NAME")
        val hooksPath = env.get("ADRENOTOOLS_HOOKS_PATH")
        if (driverPath.isBlank() || driverName.isBlank() || hooksPath.isBlank()) {
            output.writeText("Direct driver probe skipped: no custom Adrenotools driver configured.\n")
            Log.i("GN-VR-FFR", output.readText().trim())
            return
        }
        val executable = File(context.applicationInfo.nativeLibraryDir, "libgn_vrffr_probe.so")
        // Do not inherit Wine/Box64 loader overrides. Copy only Turnip's settings.
        val builder = ProcessBuilder(executable.path, hooksPath, driverPath, driverName, context.cacheDir.path,
            File(context.applicationInfo.nativeLibraryDir, "libc++_shared.so").path)
            .redirectErrorStream(true).redirectOutput(output)
        val processEnv = builder.environment()
        processEnv.keys.toList().filter {
            it.startsWith("LD_") || it.startsWith("VK_") || it.startsWith("ADRENOTOOLS_") ||
                it.startsWith("TU_") || it.startsWith("MESA_")
        }.forEach(processEnv::remove)
        for (name in env) {
            if (name.startsWith("TU_") || name.startsWith("MESA_")) processEnv[name] = env.get(name)
        }
        Thread({
            var process: Process? = null
            try {
                process = builder.start()
                if (!process.waitFor(10, TimeUnit.SECONDS)) {
                    process.destroyForcibly()
                    Log.i("GN-VR-FFR", "direct probe timed out; game launch continues independently")
                } else {
                    Log.i("GN-VR-FFR", "direct probe exit=${process.exitValue()}")
                }
                // Bounded output; the complete report remains in the diagnostics directory.
                output.bufferedReader().use { reader ->
                    repeat(64) {
                        val line = reader.readLine() ?: return@use
                        Log.i("GN-VR-FFR", line.take(4000))
                    }
                }
            } catch (e: Exception) {
                Log.w("GN-VR-FFR", "Direct driver probe failed: ${e.message}")
            } finally {
                process?.destroy()
            }
        }, "VR-FFR-driver-probe").apply { isDaemon = true }.start()
    }

    private fun detectEngine(container: Container): String = runCatching {
        val root = Container.drivesIterator(container.drives).asSequence()
            .firstOrNull { it[0].equals("A", ignoreCase = true) }?.get(1)?.let(::File)
            ?.canonicalFile ?: return@runCatching "generic"
        if (!root.isDirectory) return@runCatching "generic"
        // Bounded scan; no symlink traversal outside the game, no executable execution.
        var unity = false
        var unreal = false
        root.walkTopDown().maxDepth(5).onEnter {
            it.canonicalFile == root || it.canonicalPath.startsWith(root.path + File.separator)
        }.take(4096).forEach {
            val name = it.name.lowercase(java.util.Locale.ROOT)
            if (name == "unityplayer.dll" || name == "globalgamemanagers") unity = true
            if (name.endsWith(".uproject") || name.endsWith("-win64-shipping.exe")) unreal = true
        }
        when { unity && !unreal -> "unity"; unreal && !unity -> "unreal"; else -> "generic" }
    }.getOrDefault("generic")
}
