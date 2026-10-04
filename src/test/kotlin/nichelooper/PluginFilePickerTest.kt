package nichelooper

import nichelooper.ui.PluginFilePicker
import kotlin.test.Test
import kotlin.test.assertFalse
import kotlin.test.assertTrue

class PluginFilePickerTest {
    @Test
    fun aFolderNamedAfterAPluginIsABundle() {
        assertTrue(PluginFilePicker.isPluginBundle("Dragonfly Hall Reverb.vst3"))
        assertTrue(PluginFilePicker.isPluginBundle("lsp-plugins.VST3"))
    }

    @Test
    fun theSearchFolderItselfIsNotABundle() {
        assertFalse(PluginFilePicker.isPluginBundle(".vst3"))
        assertFalse(PluginFilePicker.isPluginBundle("vst3"))
    }

    @Test
    fun otherNamesAreNotBundles() {
        assertFalse(PluginFilePicker.isPluginBundle("Presets"))
        assertFalse(PluginFilePicker.isPluginBundle("backup.vst3.old"))
        assertFalse(PluginFilePicker.isPluginBundle(""))
    }
}
