package nichelooper.ui

import java.io.File
import javax.swing.JFileChooser
import javax.swing.filechooser.FileFilter
import nichelooper.platform.UserDirs

/**
 * Swing's file chooser, because AWT's FileDialog cannot select folders on Linux and a .vst3 plugin is a
 * folder. Call it on the UI thread.
 */
object PluginFilePicker {

    /** The folder the user chose, or null if the dialog was cancelled. */
    fun pick(): String? {
        val chooser = object : JFileChooser(startFolder()) {
            // To the user a .vst3 folder is one thing: a double click chooses it instead of opening it.
            override fun isTraversable(f: File?): Boolean = f != null && f.isDirectory && !isPluginBundle(f.name)
        }
        chooser.dialogTitle = "Choose a VST3 plugin (a .vst3 folder)"
        chooser.fileSelectionMode = JFileChooser.FILES_AND_DIRECTORIES
        chooser.isAcceptAllFileFilterUsed = false
        chooser.fileFilter = object : FileFilter() {
            override fun accept(f: File?): Boolean = f != null && f.isDirectory
            override fun getDescription(): String = "VST3 plugins (*.vst3)"
        }
        if (chooser.showOpenDialog(null) != JFileChooser.APPROVE_OPTION) return null
        return chooser.selectedFile?.absolutePath
    }

    // The search folder ~/.vst3 ends in ".vst3" too, but is not a plugin.
    internal fun isPluginBundle(name: String): Boolean =
        name.length > PLUGIN_SUFFIX.length && name.endsWith(PLUGIN_SUFFIX, ignoreCase = true)

    private const val PLUGIN_SUFFIX = ".vst3"

    private fun startFolder(): File {
        val home = UserDirs.home()
        return listOf(File(home, ".vst3"), File("/usr/lib/vst3"), File("/usr/local/lib/vst3"))
            .firstOrNull { it.isDirectory } ?: home
    }
}
