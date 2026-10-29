package nichelooper.ui

import androidx.compose.ui.input.key.Key
import nichelooper.platform.Platform
import nichelooper.platform.UserDirs
import java.io.File

/**
 * Tempo jumps: Shift + [Key] sets the BPM stored for that key (e.g. Shift+N → 99 BPM).
 * Edited in the BPM-Tasten dialog (Ctrl+B, then V). Shift keeps them apart from the plain
 * key bindings, so N can still have its own action.
 */
data class BpmKeys(val tempos: Map<Key, Int> = emptyMap()) {
    fun bpmFor(key: Key): Int? = tempos[key]

    /** Keys sorted by tempo, as listed in the dialog. */
    val entries: List<Pair<Key, Int>>
        get() = tempos.entries.sortedWith(compareBy({ it.value }, { KeyNames.displayName(it.key) }))
            .map { it.key to it.value }

    fun with(key: Key, bpm: Int): BpmKeys =
        BpmKeys(tempos + (key to bpm.coerceIn(TransportViewModel.MIN_BPM, TransportViewModel.MAX_BPM)))

    fun without(key: Key): BpmKeys = BpmKeys(tempos - key)

    fun serialize(): String = buildString {
        appendLine("# NicheLooper BPM keys (Shift + key = tempo)")
        for ((key, bpm) in entries) {
            appendLine("${KeyNames.toToken(key)}=$bpm")
        }
    }

    companion object {
        fun parse(text: String?): BpmKeys {
            if (text.isNullOrBlank()) return BpmKeys()
            var keys = BpmKeys()
            for (rawLine in text.lineSequence()) {
                val line = rawLine.trim()
                if (line.isEmpty() || line.startsWith("#")) continue
                val eq = line.indexOf('=')
                if (eq <= 0) continue
                val key = KeyNames.fromToken(line.substring(0, eq).trim()) ?: continue
                val bpm = line.substring(eq + 1).trim().toIntOrNull() ?: continue
                keys = keys.with(key, bpm)
            }
            return keys
        }
    }
}

/**
 * Persistence for [BpmKeys], next to the key bindings.
 */
object BpmKeysStore {
    private val file: File =
        if (Platform.isMac) {
            File(System.getProperty("user.home"), "Library/Application Support/NicheLooper/bpm-keys.properties")
        } else {
            File(UserDirs.dataHome(), "NicheLooper/bpm-keys.properties")
        }

    fun load(): BpmKeys =
        runCatching { BpmKeys.parse(file.takeIf { it.isFile }?.readText()) }
            .getOrDefault(BpmKeys())

    fun save(keys: BpmKeys): Boolean = runCatching {
        val dir = file.parentFile
        if (dir != null && !dir.isDirectory) dir.mkdirs()
        file.writeText(keys.serialize())
        true
    }.getOrDefault(false)
}
