package nichelooper.audio

import java.nio.file.Files

/**
 * Kotlin facade over the native (C++/miniaudio-CoreAudio) engine in
 * libnichelooper.dylib.
 *
 * All calls are cheap: they either flip atomics read by the audio thread or
 * read atomics written by it. start()/stop() are the only heavy calls and
 * must not be invoked from the main thread in a tight loop.
 */
object AudioEngine {

    init {
        loadNativeLibrary()
    }

    // The dylib is packaged as a classpath resource (built by the Gradle
    // buildNative task); extract it to a temp file so System.load works both
    // from the exploded dev classpath and from a packaged jar/.app.
    private fun loadNativeLibrary() {
        val resource = AudioEngine::class.java.getResourceAsStream("/native/libnichelooper.dylib")
            ?: error("libnichelooper.dylib fehlt im Classpath — Gradle-Build ausführen (Task buildNative).")
        val temp = Files.createTempFile("libnichelooper", ".dylib")
        resource.use { input -> Files.newOutputStream(temp).use { output -> input.copyTo(output) } }
        temp.toFile().deleteOnExit()
        System.load(temp.toAbsolutePath().toString())
    }

    // ---- Device enumeration (indices are positions in the last refresh) ----

    /** Re-enumerates CoreAudio devices. Returns false if the context failed. */
    fun refreshDevices(): Boolean = nativeRefreshDevices()

    val inputDeviceNames: List<String> get() = nativeGetInputDeviceNames().toList()
    val outputDeviceNames: List<String> get() = nativeGetOutputDeviceNames().toList()
    val defaultInputIndex: Int get() = nativeGetDefaultInputIndex()
    val defaultOutputIndex: Int get() = nativeGetDefaultOutputIndex()

    /**
     * Opens and starts the duplex device on the given device indices from the
     * last [refreshDevices] enumeration (< 0 = system default). Returns false
     * if the device could not be opened/started.
     */
    fun start(inputIndex: Int, outputIndex: Int): Boolean = nativeStart(inputIndex, outputIndex)

    fun stop() = nativeStop()

    val isRunning: Boolean get() = nativeIsRunning()

    /** True after the audio device disappeared; the engine must be stopped. */
    val isDisconnected: Boolean get() = nativeIsDisconnected()

    fun sendCommand(command: LooperCommand) = nativeSendCommand(command.nativeValue)

    val looperState: LooperState get() = LooperState.fromNative(nativeGetState())
    val positionFrames: Int get() = nativeGetPositionFrames()
    val loopLengthFrames: Int get() = nativeGetLoopLengthFrames()
    val maxLoopFrames: Int get() = nativeGetMaxLoopFrames()
    val sampleRate: Int get() = nativeGetSampleRate()
    val framesPerBurst: Int get() = nativeGetFramesPerBurst()

    /**
     * Snapshot of the current loop as mono float samples, or null if there
     * is no loop. Safe to call while the engine keeps running.
     */
    fun copyLoop(): FloatArray? {
        val length = loopLengthFrames
        if (length <= 0) return null
        val samples = FloatArray(length)
        val copied = nativeCopyLoop(samples)
        if (copied <= 0) return null
        return if (copied == length) samples else samples.copyOf(copied)
    }

    /**
     * Replaces the current loop with the given mono samples (at the engine
     * sample rate) and starts playback. Requires a running engine.
     */
    fun loadLoop(samples: FloatArray): Boolean = nativeLoadLoop(samples)

    /** Peak |sample| since the last call (reading resets the meter). */
    fun readInputPeak(): Float = nativeReadInputPeak()

    /** Post-chain peak — differs from the input peak when plugins are active. */
    fun readFxPeak(): Float = nativeReadFxPeak()
    fun readOutputPeak(): Float = nativeReadOutputPeak()

    fun setMonitor(enabled: Boolean) = nativeSetMonitor(enabled)
    fun setInputGain(gain: Float) = nativeSetInputGain(gain)
    fun setOutputGain(gain: Float) = nativeSetOutputGain(gain)

    // Rhythm section (metronome click + drum machine).
    fun setBpm(bpm: Int) = nativeSetBpm(bpm)
    fun setMetronome(enabled: Boolean) = nativeSetMetronome(enabled)
    fun setDrums(enabled: Boolean) = nativeSetDrums(enabled)

    /**
     * Silences the drums while the bar clock keeps running — a live "drop
     * out for a bar" gesture. Unmuting re-enters the groove at the current
     * step, so it always lands in time. Hits already sounding ring out; the
     * metronome is not affected.
     */
    fun setDrumsMuted(muted: Boolean) = nativeSetDrumsMuted(muted)

    /** Index into [RhythmTables.timeSignatureNames]; sets bar length + accents. */
    fun setTimeSignature(index: Int) = nativeSetTimeSignature(index)

    /**
     * Index into [RhythmTables.grooveNames] — the drum pattern played inside
     * the bar. A groove written for a different signature than the current
     * one is ignored by the engine (it falls back to that signature's first
     * groove), so the two calls can be made in either order.
     */
    fun setGroove(index: Int) = nativeSetGroove(index)

    /**
     * The engine's built-in signature and groove tables. Constant, but read
     * from native so the UI cannot drift out of sync with RhythmSection.cpp.
     * Safe to call before start().
     */
    fun rhythmTables(): RhythmTables = RhythmTables(
        timeSignatureNames = nativeGetTimeSignatureNames().toList(),
        beatsPerBar = nativeGetBeatsPerBar().toList(),
        grooveNames = nativeGetGrooveNames().toList(),
        grooveTimeSignature = nativeGetGrooveTimeSignatures().toList(),
    )

    fun setRhythmVolume(volume: Float) = nativeSetRhythmVolume(volume)
    fun setCountIn(enabled: Boolean) = nativeSetCountIn(enabled)

    /** Playback volume of the loop track (recording stays full-scale). */
    fun setLoopVolume(volume: Float) = nativeSetLoopVolume(volume)

    /** Recording auto-closes after this many bars; 0 disables auto-loop. */
    fun setAutoLoopBars(bars: Int) = nativeSetAutoLoopBars(bars)

    /**
     * Stages real drum one-shots (mono float at [sourceRate]) for the drum
     * machine. Call before start(); the engine converts them to its own
     * sample rate when the streams open.
     */
    fun setDrumSamples(kick: FloatArray, snare: FloatArray, hat: FloatArray, sourceRate: Int) =
        nativeSetDrumSamples(kick, snare, hat, sourceRate)

    // ---- Plugin chains (Audio Unit hosting) ----
    // Three chains process the live input in front of the looper; the active
    // one is switched glitch-free (keys A/S/D). Plugin indices refer to the
    // most recent [effectPluginNames] enumeration.

    val effectPluginNames: List<String> get() = nativeGetEffectPluginNames().toList()

    fun chainAddPlugin(chain: Int, pluginIndex: Int): Boolean =
        nativeChainAddPlugin(chain, pluginIndex)

    fun chainRemovePlugin(chain: Int, slot: Int): Boolean =
        nativeChainRemovePlugin(chain, slot)

    fun chainMovePlugin(chain: Int, from: Int, to: Int): Boolean =
        nativeChainMovePlugin(chain, from, to)

    fun chainPluginNames(chain: Int): List<String> =
        nativeChainGetPluginNames(chain).toList()

    /** Opens (or refocuses) the plugin's own editor window. */
    fun openPluginEditor(chain: Int, slot: Int) = nativeOpenPluginEditor(chain, slot)

    fun setActiveChain(index: Int) = nativeSetActiveChain(index)
    val activeChain: Int get() = nativeGetActiveChain()

    /** Render/error counters per plugin — ground truth for chain debugging. */
    val chainDebugReport: String get() = nativeGetChainDebugReport()

    // ---- Preset bank (all 3 chains) ----

    /** Snapshots all 3 chains (plugin identity + full AU state, incl. NAM model) as bytes. */
    fun saveBank(): ByteArray = nativeSaveBank()

    /** Replaces all 3 chains from a previously saved bank. Requires a running engine. */
    fun loadBank(data: ByteArray): Boolean = nativeLoadBank(data)

    private external fun nativeRefreshDevices(): Boolean
    private external fun nativeGetInputDeviceNames(): Array<String>
    private external fun nativeGetOutputDeviceNames(): Array<String>
    private external fun nativeGetDefaultInputIndex(): Int
    private external fun nativeGetDefaultOutputIndex(): Int
    private external fun nativeStart(inputIndex: Int, outputIndex: Int): Boolean
    private external fun nativeStop()
    private external fun nativeIsRunning(): Boolean
    private external fun nativeIsDisconnected(): Boolean
    private external fun nativeSendCommand(command: Int)
    private external fun nativeGetState(): Int
    private external fun nativeGetPositionFrames(): Int
    private external fun nativeGetLoopLengthFrames(): Int
    private external fun nativeGetMaxLoopFrames(): Int
    private external fun nativeGetSampleRate(): Int
    private external fun nativeGetFramesPerBurst(): Int
    private external fun nativeCopyLoop(dest: FloatArray): Int
    private external fun nativeLoadLoop(samples: FloatArray): Boolean
    private external fun nativeReadInputPeak(): Float
    private external fun nativeReadFxPeak(): Float
    private external fun nativeReadOutputPeak(): Float
    private external fun nativeSetMonitor(enabled: Boolean)
    private external fun nativeSetInputGain(gain: Float)
    private external fun nativeSetOutputGain(gain: Float)
    private external fun nativeSetBpm(bpm: Int)
    private external fun nativeSetMetronome(enabled: Boolean)
    private external fun nativeSetDrums(enabled: Boolean)
    private external fun nativeSetDrumsMuted(muted: Boolean)
    private external fun nativeSetTimeSignature(index: Int)
    private external fun nativeSetGroove(index: Int)
    private external fun nativeGetTimeSignatureNames(): Array<String>
    private external fun nativeGetBeatsPerBar(): IntArray
    private external fun nativeGetGrooveNames(): Array<String>
    private external fun nativeGetGrooveTimeSignatures(): IntArray
    private external fun nativeSetRhythmVolume(volume: Float)
    private external fun nativeSetCountIn(enabled: Boolean)
    private external fun nativeSetLoopVolume(volume: Float)
    private external fun nativeSetAutoLoopBars(bars: Int)
    private external fun nativeSetDrumSamples(
        kick: FloatArray,
        snare: FloatArray,
        hat: FloatArray,
        sourceRate: Int,
    )
    private external fun nativeGetEffectPluginNames(): Array<String>
    private external fun nativeChainAddPlugin(chain: Int, pluginIndex: Int): Boolean
    private external fun nativeChainRemovePlugin(chain: Int, slot: Int): Boolean
    private external fun nativeChainMovePlugin(chain: Int, from: Int, to: Int): Boolean
    private external fun nativeChainGetPluginNames(chain: Int): Array<String>
    private external fun nativeOpenPluginEditor(chain: Int, slot: Int)
    private external fun nativeSetActiveChain(index: Int)
    private external fun nativeGetActiveChain(): Int
    private external fun nativeGetChainDebugReport(): String
    private external fun nativeSaveBank(): ByteArray
    private external fun nativeLoadBank(data: ByteArray): Boolean
}
