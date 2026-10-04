package nichelooper.audio

import nichelooper.platform.Platform
import nichelooper.platform.UserDirs
import java.io.File

/**
 * Named A/S/D chain presets. Each preset is the blob produced by
 * [AudioEngine.saveBank] — i.e. a snapshot of all three plugin chains
 * (plugin identity + full plugin state, incl. the loaded NAM model): on macOS
 * a binary property list (AudioComponentDescription + AU ClassInfo), on Linux
 * a "NLK1" blob of the VST3 component/controller states. Presets are not
 * portable between the two. Stored one file per preset in
 * ~/Library/Application Support/NicheLooper/presets (macOS) or
 * ~/.local/share/NicheLooper/presets (Linux, honouring XDG_DATA_HOME).
 *
 * Nothing is auto-saved: presets exist only after the user explicitly saves
 * one via the top-right menu.
 */
object ChainPresets {

    private val dir =
        if (Platform.isMac) {
            File(System.getProperty("user.home"), "Library/Application Support/NicheLooper/presets")
        } else {
            File(UserDirs.dataHome(), "NicheLooper/presets")
        }
    private const val EXT = "namchain"

    private fun file(name: String): File = File(dir, sanitize(name) + ".$EXT")

    /** All saved preset names, sorted (empty until the user saves one). */
    fun list(): List<String> {
        if (!dir.isDirectory) return emptyList()
        return dir.listFiles { f -> f.isFile && f.extension.equals(EXT, true) }
            ?.map { stripExt(it.name) }
            ?.sortedBy { it.lowercase() }
            ?: emptyList()
    }

    fun exists(name: String): Boolean = file(name).isFile

    /** Overwrites a preset of the same name. Returns false on IO failure. */
    fun save(name: String, data: ByteArray): Boolean = runCatching {
        check(dir.isDirectory || dir.mkdirs()) { "Kann ${dir.absolutePath} nicht anlegen" }
        check(data.isNotEmpty()) { "Leere Preset-Daten" }
        file(name).writeBytes(data)
        true
    }.getOrDefault(false)

    fun load(name: String): ByteArray? = file(name).takeIf { it.isFile }?.readBytes()

    fun delete(name: String): Boolean = file(name).delete()

    private fun sanitize(name: String): String {
        val trimmed = name.trim()
        if (trimmed.isEmpty()) return "preset"
        val cleaned = trimmed.replace(Regex("[/\\\\:*?\"<>|]"), "_")
        return cleaned.ifBlank { "preset" }
    }

    private fun stripExt(fileName: String): String =
        fileName.substringBeforeLast(".$EXT", fileName)
}
