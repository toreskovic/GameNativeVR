package com.winlator.container

import androidx.compose.runtime.saveable.SaverScope
import org.json.JSONObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

@RunWith(RobolectricTestRunner::class)
class VrFxaaConfigTest {
    private val scope = object : SaverScope {
        override fun canBeSaved(value: Any) = true
    }

    @Test fun packedTransportDefaultsOnAndPersistsOptOut() {
        val container = Container("packing-test")
        container.loadData(JSONObject())
        assertTrue(container.xrPackedTransport)
        assertTrue(ContainerData().xrPackedTransport)
        for (enabled in listOf(true, false)) {
            container.loadData(JSONObject().put("xrPackedTransport", enabled))
            assertEquals(enabled, container.xrPackedTransport)
            val saved = with(ContainerData.Saver) {
                scope.save(ContainerData(xrPackedTransport = enabled))
            }!!
            assertEquals(enabled, ContainerData.Saver.restore(saved)!!.xrPackedTransport)
        }
    }

    @Test fun oldContainerDefaultsOnButExplicitOffIsRespected() {
        val legacy = Container("fxaa-test")
        legacy.loadData(JSONObject())
        assertTrue(legacy.xrFxaa)
        val disabled = Container("fxaa-test")
        disabled.loadData(JSONObject().put("xrFxaa", false))
        assertFalse(disabled.xrFxaa)
    }

    @Test fun ffrDebugPersistsAndRejectsInvalidModes() {
        for(mode in 0..3) {
            val container=Container("debug-test")
            container.loadData(JSONObject().put("xrFfrDebug",mode))
            assertEquals(mode,container.xrFfrDebug)
            val saved=with(ContainerData.Saver) { scope.save(ContainerData(xrFfrDebug=mode)) }!!
            assertEquals(mode,ContainerData.Saver.restore(saved)!!.xrFfrDebug)
        }
        val container=Container("debug-test")
        container.loadData(JSONObject().put("xrFfrDebug",42))
        assertEquals(0,container.xrFfrDebug)
    }

    @Test fun editorStatePreservesBothValues() {
        for (enabled in listOf(false, true)) {
            val state = ContainerData(xrFxaa = enabled)
            val saved = with(ContainerData.Saver) { scope.save(state) }!!
            val restored = ContainerData.Saver.restore(saved)!!
            if (enabled) assertTrue(restored.xrFxaa) else assertFalse(restored.xrFxaa)
        }
        assertTrue(ContainerData().xrFxaa)
    }
    @Test fun retiredVrGenerationSettingsAreIgnoredByEditor() {
        val current = with(ContainerData.Saver) {
            scope.save(ContainerData(windowsVrEnabled = true, lsfgEnabled = true))
        } as List<*>
        // mapSaver serializes its map as alternating keys and values.
        val old = current + listOf("lsfgVrFlowScale", 25, "vrFrameGenerationBackend", "fidelityfx")
        val restored = ContainerData.Saver.restore(old)!!
        assertTrue(restored.windowsVrEnabled)
        // Preserve the independent flat-screen LSFG preference.
        assertTrue(restored.lsfgEnabled)
        val saved = with(ContainerData.Saver) { scope.save(restored) } as List<*>
        assertFalse(saved.contains("lsfgVrFlowScale"))
        assertFalse(saved.contains("vrFrameGenerationBackend"))
    }

}
