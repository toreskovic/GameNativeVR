package app.gamenative.utils

import android.content.Context
import app.gamenative.BuildConfig
import app.gamenative.PrefManager
import com.winlator.core.DefaultVersion
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import okhttp3.Request
import timber.log.Timber

object ManifestRepository {
    private const val ONE_DAY_MS = 24 * 60 * 60 * 1000L
    private const val MANIFEST_URL = "https://raw.githubusercontent.com/utkarshdalal/GameNative/refs/heads/master/manifest.json"
    private val json = Json { ignoreUnknownKeys = true }

    suspend fun loadManifest(context: Context): ManifestData {
        if (BuildConfig.DEBUG) {
            readLocalManifest(context)?.let {
                Timber.i("ManifestRepository: using local debug manifest")
                return withXrComponents(context, it)
            }
        }

        val cachedJson = PrefManager.componentManifestJson
        val cachedManifest = parseManifest(cachedJson) ?: ManifestData.empty()
        val lastFetchedAt = PrefManager.componentManifestFetchedAt
        val isStale = System.currentTimeMillis() - lastFetchedAt >= ONE_DAY_MS

        if (cachedJson.isNotEmpty() && !isStale) {
            return withXrComponents(context, cachedManifest)
        }

        val fetched = fetchManifestJson()
        if (fetched != null) {
            val parsed = parseManifest(fetched)
            if (parsed != null) {
                val now = System.currentTimeMillis()
                PrefManager.componentManifestJson = fetched
                PrefManager.componentManifestFetchedAt = now
                return withXrComponents(context, parsed)
            }
        }

        return withXrComponents(context, cachedManifest)
    }

    private fun withXrComponents(context: Context, manifest: ManifestData): ManifestData {
        if (!BuildConfig.XR_BUILD) return manifest
        val bundled = try {
            context.assets.open("xr-manifest.json").bufferedReader().use { parseManifest(it.readText()) }
        } catch (e: Exception) {
            Timber.w(e, "ManifestRepository: bundled XR manifest unavailable")
            null
        }
        if (bundled == null) return manifest
        val items = manifest.items.toMutableMap()
        // Keep this fork's pinned defaults available even if upstream omits or changes them.
        val overrides = mapOf(
            ManifestContentTypes.DRIVER to ContainerUtils.WRAPPER_PICO_A10,
            ManifestContentTypes.PROTON to DefaultVersion.WINE_VERSION,
        )
        for ((type, id) in overrides) {
            val entry = bundled.items[type]?.firstOrNull { it.id == id } ?: continue
            items[type] = listOf(entry) + items[type].orEmpty().filterNot { it.id == id }
        }
        return manifest.copy(items = items)
    }

    private suspend fun fetchManifestJson(): String? = withContext(Dispatchers.IO) {
    try {
        val request = Request.Builder().url(MANIFEST_URL).build()
        Net.http.newCall(request).execute().use { response ->
            response.takeIf { it.isSuccessful }?.body?.string()
        }
    } catch (e: Exception) {
        Timber.e(e, "ManifestRepository: fetch failed")
        null
    }
}

    private fun readLocalManifest(context: Context): ManifestData? = try {
        context.assets.open("manifest.json").bufferedReader().use { parseManifest(it.readText()) }
    } catch (e: Exception) {
        null
    }

    fun parseManifest(jsonString: String?): ManifestData? {
        if (jsonString.isNullOrBlank()) return null
        return try {
            json.decodeFromString<ManifestData>(jsonString)
        } catch (e: Exception) {
            Timber.e(e, "ManifestRepository: parse failed")
            null
        }
    }
}
