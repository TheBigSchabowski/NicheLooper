package nichelooper.audio

/**
 * Snapshot of the engine's built-in time signatures and drum grooves, read
 * once from native via [AudioEngine.rhythmTables]. The tables are compile-time
 * constants in RhythmSection.cpp; mirroring them here (instead of hardcoding
 * them in the UI) means adding a groove touches exactly one file.
 *
 * All lists are parallel to their index: [grooveNames] `[i]` is the name of
 * groove `i`, [grooveTimeSignature] `[i]` the signature it was written for.
 * Empty when the native library could not be queried — the UI then simply
 * shows no selectors, which is the same situation in which nothing plays.
 */
data class RhythmTables(
    val timeSignatureNames: List<String> = emptyList(),
    val beatsPerBar: List<Int> = emptyList(),
    val grooveNames: List<String> = emptyList(),
    val grooveTimeSignature: List<Int> = emptyList(),
) {
    /** Beats per bar of [timeSignature]; 4 as a display fallback. */
    fun beatsPerBar(timeSignature: Int): Int = beatsPerBar.getOrElse(timeSignature) { 4 }

    /** Groove indices playable in [timeSignature], in engine order. */
    fun groovesFor(timeSignature: Int): List<Int> =
        grooveTimeSignature.indices.filter { grooveTimeSignature[it] == timeSignature }

    fun grooveName(index: Int): String = grooveNames.getOrElse(index) { "—" }
}
