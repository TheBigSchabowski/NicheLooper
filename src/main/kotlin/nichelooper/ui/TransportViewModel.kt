package nichelooper.ui

import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import nichelooper.audio.AudioEngine
import nichelooper.audio.ChainPresets
import nichelooper.audio.DrumKitLoader
import nichelooper.audio.LoopLibrary
import nichelooper.audio.LoopSaver
import nichelooper.audio.LooperCommand
import nichelooper.audio.LooperState
import nichelooper.audio.RhythmTables
import nichelooper.audio.SavedLoop

data class TransportUiState(
    val inputDevices: List<String> = emptyList(),
    val outputDevices: List<String> = emptyList(),
    val selectedInput: String? = null,
    val selectedOutput: String? = null,
    val availablePlugins: List<String> = emptyList(),
    val chains: List<List<String>> = List(TransportViewModel.CHAIN_COUNT) { emptyList() },
    val activeChain: Int = 0,
    val engineRunning: Boolean = false,
    val looperState: LooperState = LooperState.EMPTY,
    val positionFrames: Int = 0,
    val loopLengthFrames: Int = 0,
    val maxLoopFrames: Int = 0,
    val sampleRate: Int = 0,
    val framesPerBurst: Int = 0,
    val monitorEnabled: Boolean = true,
    val inputGain: Float = 1f,
    val outputGain: Float = 1f,
    val bpm: Int = 120,
    val metronomeEnabled: Boolean = false,
    val drumsEnabled: Boolean = false,
    val drumsMuted: Boolean = false,
    val rhythm: RhythmTables = RhythmTables(),
    val timeSignature: Int = 0,          // index into rhythm.timeSignatureNames
    val groove: Int = 0,                 // index into rhythm.grooveNames
    val countInEnabled: Boolean = false,
    val rhythmVolume: Float = 1f,
    val loopVolume: Float = 1f,
    val autoLoopEnabled: Boolean = false,
    val autoLoopBars: Int = 4,           // 4, 6 or 8
    val inputPeak: Float = 0f,
    val fxPeak: Float = 0f,
    val outputPeak: Float = 0f,
    val saving: Boolean = false,
    val saveMessage: String? = null,
    val savedLoops: List<SavedLoop> = emptyList(),
    val presets: List<String> = emptyList(),
    val loadingLoop: Boolean = false,
    val errorMessage: String? = null,
)

/**
 * Desktop counterpart of the Android TransportViewModel. Devices are chosen
 * by name from the CoreAudio enumeration (instead of the USB detector);
 * selection survives re-enumeration because names, not indices, are stored.
 * The engine is started explicitly via the Start button — auto-starting on
 * the built-in mic + speakers with monitoring on would howl.
 */
class TransportViewModel {

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)

    private val _uiState = MutableStateFlow(TransportUiState())
    val uiState: StateFlow<TransportUiState> = _uiState.asStateFlow()

    private val startInProgress = AtomicBoolean(false)

    init {
        // Watch the device list (hot-plug); also does the very first
        // enumeration, which loads the native library.
        scope.launch {
            while (isActive) {
                refreshDevices()
                delay(DEVICE_POLL_MS)
            }
        }

        // One-time read of the engine's signature/groove tables for the menus.
        scope.launch {
            val tables = runCatching { AudioEngine.rhythmTables() }.getOrNull()
            if (tables != null) {
                _uiState.update { it.copy(rhythm = tables) }
            }
        }

        // One-time enumeration of installed effect AUs for the chain editor.
        scope.launch {
            val plugins = runCatching { AudioEngine.effectPluginNames }
                .getOrDefault(emptyList())
            _uiState.update { it.copy(availablePlugins = plugins) }
        }

        // Populate the preset menu (bank may be empty on a fresh install).
        refreshPresets()

        // Poll the engine for playhead/state; also catches disconnects
        // reported from the native notification callback.
        scope.launch {
            while (isActive) {
                delay(POLL_MS)
                if (!_uiState.value.engineRunning) continue
                if (AudioEngine.isDisconnected || !AudioEngine.isRunning) {
                    stopEngine("Audio device stopped (unplugged or reconfigured). Press Start to restart.")
                } else {
                    val inPeak = AudioEngine.readInputPeak()
                    val fxPeak = AudioEngine.readFxPeak()
                    val outPeak = AudioEngine.readOutputPeak()
                    _uiState.update {
                        it.copy(
                            looperState = AudioEngine.looperState,
                            positionFrames = AudioEngine.positionFrames,
                            loopLengthFrames = AudioEngine.loopLengthFrames,
                            // simple meter ballistics: instant attack, ~0.25s release
                            inputPeak = maxOf(inPeak, it.inputPeak * 0.8f),
                            fxPeak = maxOf(fxPeak, it.fxPeak * 0.8f),
                            outputPeak = maxOf(outPeak, it.outputPeak * 0.8f),
                        )
                    }
                }
            }
        }
    }

    private fun refreshDevices() {
        val result = runCatching {
            check(AudioEngine.refreshDevices()) { "CoreAudio context failed" }
        }
        if (result.isFailure) {
            _uiState.update {
                it.copy(errorMessage = "Audio engine unavailable: ${result.exceptionOrNull()?.message}")
            }
            return
        }
        val inputs = AudioEngine.inputDeviceNames
        val outputs = AudioEngine.outputDeviceNames
        val defaultIn = AudioEngine.defaultInputIndex
        val defaultOut = AudioEngine.defaultOutputIndex
        _uiState.update { s ->
            s.copy(
                inputDevices = inputs,
                outputDevices = outputs,
                selectedInput = s.selectedInput?.takeIf { it in inputs }
                    ?: inputs.getOrNull(defaultIn) ?: inputs.firstOrNull(),
                selectedOutput = s.selectedOutput?.takeIf { it in outputs }
                    ?: outputs.getOrNull(defaultOut) ?: outputs.firstOrNull(),
            )
        }
    }

    fun startEngine() {
        if (_uiState.value.engineRunning) return
        if (!startInProgress.compareAndSet(false, true)) return

        scope.launch {
            try {
                // Real drum samples must be staged before the streams open.
                DrumKitLoader.ensureLoaded()
                // Resolve the selected names against a fresh enumeration; a
                // vanished device falls back to the system default (-1).
                AudioEngine.refreshDevices()
                val state = _uiState.value
                val inputIndex = state.selectedInput
                    ?.let { AudioEngine.inputDeviceNames.indexOf(it) } ?: -1
                val outputIndex = state.selectedOutput
                    ?.let { AudioEngine.outputDeviceNames.indexOf(it) } ?: -1
                val ok = AudioEngine.start(inputIndex, outputIndex)
                if (ok) {
                    // Re-apply UI-side settings to the fresh engine.
                    val s = _uiState.value
                    AudioEngine.setMonitor(s.monitorEnabled)
                    AudioEngine.setInputGain(s.inputGain)
                    AudioEngine.setOutputGain(s.outputGain)
                    AudioEngine.setBpm(s.bpm)
                    AudioEngine.setMetronome(s.metronomeEnabled)
                    AudioEngine.setDrums(s.drumsEnabled)
                    AudioEngine.setDrumsMuted(s.drumsMuted)
                    AudioEngine.setTimeSignature(s.timeSignature)
                    AudioEngine.setGroove(s.groove)
                    AudioEngine.setCountIn(s.countInEnabled)
                    AudioEngine.setRhythmVolume(s.rhythmVolume)
                    AudioEngine.setLoopVolume(s.loopVolume)
                    AudioEngine.setAutoLoopBars(if (s.autoLoopEnabled) s.autoLoopBars else 0)
                    _uiState.update {
                        it.copy(
                            engineRunning = true,
                            looperState = LooperState.EMPTY,
                            positionFrames = 0,
                            loopLengthFrames = 0,
                            maxLoopFrames = AudioEngine.maxLoopFrames,
                            sampleRate = AudioEngine.sampleRate,
                            framesPerBurst = AudioEngine.framesPerBurst,
                            errorMessage = null,
                        )
                    }
                } else {
                    _uiState.update {
                        it.copy(
                            engineRunning = false,
                            errorMessage = "Could not open duplex audio streams on " +
                                "\"${state.selectedInput ?: "default"}\" → " +
                                "\"${state.selectedOutput ?: "default"}\". " +
                                "Check the microphone permission in System Settings.",
                        )
                    }
                }
            } finally {
                startInProgress.set(false)
            }
        }
    }

    fun stopEngineByUser() = stopEngine(null)

    private fun stopEngine(error: String?) {
        scope.launch {
            AudioEngine.stop()
            _uiState.update {
                it.copy(
                    engineRunning = false,
                    looperState = LooperState.EMPTY,
                    positionFrames = 0,
                    loopLengthFrames = 0,
                    errorMessage = error,
                )
            }
        }
    }

    fun selectInputDevice(name: String) {
        _uiState.update { it.copy(selectedInput = name) }
        restartIfRunning()
    }

    fun selectOutputDevice(name: String) {
        _uiState.update { it.copy(selectedOutput = name) }
        restartIfRunning()
    }

    // A device change while running moves the whole duplex engine (the
    // current loop is lost, as on Android when re-plugging USB).
    private fun restartIfRunning() {
        if (!_uiState.value.engineRunning) return
        scope.launch {
            AudioEngine.stop()
            _uiState.update { it.copy(engineRunning = false) }
            startEngine()
        }
    }

    /** Refreshes the saved-loop list (call before showing the picker). */
    fun refreshSavedLoops() {
        scope.launch(Dispatchers.IO) {
            val loops = runCatching { LoopLibrary.list() }.getOrDefault(emptyList())
            _uiState.update { it.copy(savedLoops = loops) }
        }
    }

    /** Decodes the file and swaps it into the engine — playback starts immediately. */
    fun loadSavedLoop(loop: SavedLoop) {
        val state = _uiState.value
        if (state.loadingLoop || !state.engineRunning || state.sampleRate <= 0) return
        _uiState.update { it.copy(loadingLoop = true, saveMessage = null) }
        scope.launch(Dispatchers.IO) {
            val result = runCatching {
                val samples = LoopLibrary.decode(loop, state.sampleRate, AudioEngine.maxLoopFrames)
                check(AudioEngine.loadLoop(samples)) { "Engine rejected the loop" }
            }
            _uiState.update {
                it.copy(
                    loadingLoop = false,
                    saveMessage = result.fold(
                        onSuccess = { "Loaded: ${loop.name}" },
                        onFailure = { e -> "Load failed: ${e.message ?: e.javaClass.simpleName}" },
                    ),
                )
            }
        }
    }

    // ---- Plugin chains ----

    /** Switches the live chain (keys A/S/D or the chips). Glitch-free. */
    fun setActiveChain(index: Int) {
        if (index !in 0 until CHAIN_COUNT) return
        AudioEngine.setActiveChain(index)
        _uiState.update { it.copy(activeChain = index) }
    }

    /** Adds plugin [pluginIndex] (from availablePlugins) to the active chain. */
    fun addPluginToChain(pluginIndex: Int) {
        val chain = _uiState.value.activeChain
        scope.launch {
            val ok = AudioEngine.chainAddPlugin(chain, pluginIndex)
            val name = _uiState.value.availablePlugins.getOrNull(pluginIndex) ?: "?"
            _uiState.update {
                it.copy(
                    chains = readChains(),
                    saveMessage = if (ok) null else "Plugin failed to load: $name",
                )
            }
        }
    }

    fun removePluginFromChain(slot: Int) {
        val chain = _uiState.value.activeChain
        scope.launch {
            AudioEngine.chainRemovePlugin(chain, slot)
            _uiState.update { it.copy(chains = readChains()) }
        }
    }

    fun movePluginUp(slot: Int) {
        val chain = _uiState.value.activeChain
        if (slot <= 0) return
        scope.launch {
            AudioEngine.chainMovePlugin(chain, slot, slot - 1)
            _uiState.update { it.copy(chains = readChains()) }
        }
    }

    /** Opens the plugin's own editor window (e.g. to load a NAM model). */
    fun openPluginEditor(slot: Int) {
        val chain = _uiState.value.activeChain
        scope.launch { AudioEngine.openPluginEditor(chain, slot) }
    }

    private fun readChains(): List<List<String>> =
        (0 until CHAIN_COUNT).map { AudioEngine.chainPluginNames(it) }

    // ---- Named A/S/D presets ----

    /** Loads the saved-preset list (call before opening the menu). */
    fun refreshPresets() {
        scope.launch(Dispatchers.IO) {
            val names = runCatching { ChainPresets.list() }.getOrDefault(emptyList())
            _uiState.update { it.copy(presets = names) }
        }
    }

    /**
     * Snapshots the current 3 chains and stores them under [name]. If a preset
     * with that name already exists, it is overwritten in place (this is the
     * "edit an existing preset" path — the change sticks for the next session).
     */
    fun savePreset(name: String) {
        val trimmed = name.trim()
        if (trimmed.isEmpty()) return
        if (!_uiState.value.engineRunning) {
            _uiState.update { it.copy(saveMessage = "Preset speichern braucht laufende Engine.") }
            return
        }
        scope.launch(Dispatchers.IO) {
            val existed = ChainPresets.exists(trimmed)
            val result = runCatching {
                val data = AudioEngine.saveBank()
                check(data.isNotEmpty()) { "Keine Chain-Daten (Engine/Plugin-Fehler)" }
                check(ChainPresets.save(trimmed, data)) { "Speichern fehlgeschlagen" }
            }
            refreshPresets()
            _uiState.update {
                it.copy(saveMessage = result.fold(
                    onSuccess = {
                        if (existed) "Preset aktualisiert: $trimmed"
                        else "Preset gespeichert: $trimmed"
                    },
                    onFailure = { e -> "Preset-Fehler: ${e.message ?: e.javaClass.simpleName}" },
                ))
            }
        }
    }

    /** Restores all 3 chains from preset [name]. Requires a running engine. */
    fun loadPreset(name: String) {
        if (!_uiState.value.engineRunning) {
            _uiState.update { it.copy(saveMessage = "Preset laden braucht laufende Engine.") }
            return
        }
        scope.launch(Dispatchers.IO) {
            val result = runCatching {
                val data = ChainPresets.load(name) ?: error("Preset nicht gefunden: $name")
                check(AudioEngine.loadBank(data)) { "Bank konnte nicht geladen werden" }
            }
            _uiState.update {
                it.copy(
                    chains = readChains(),
                    activeChain = AudioEngine.activeChain,
                    saveMessage = result.fold(
                        onSuccess = { "Preset geladen: $name" },
                        onFailure = { e -> "Preset-Fehler: ${e.message ?: e.javaClass.simpleName}" },
                    ),
                )
            }
        }
    }

    fun deletePreset(name: String) {
        scope.launch(Dispatchers.IO) {
            ChainPresets.delete(name)
            refreshPresets()
            _uiState.update { it.copy(saveMessage = "Preset gelöscht: $name" ) }
        }
    }

    fun record() = AudioEngine.sendCommand(LooperCommand.RECORD)
    fun play() = AudioEngine.sendCommand(LooperCommand.PLAY)
    fun toggleOverdub() = AudioEngine.sendCommand(LooperCommand.OVERDUB)
    fun stopLoop() = AudioEngine.sendCommand(LooperCommand.STOP)
    fun clear() {
        AudioEngine.sendCommand(LooperCommand.CLEAR)
        _uiState.update { it.copy(saveMessage = null) }
    }

    /** Snapshots the loop and writes it to ~/Music/NicheLooper — audio keeps running. */
    fun saveLoop() {
        val state = _uiState.value
        if (state.saving || state.loopLengthFrames <= 0 || state.sampleRate <= 0) return
        val samples = AudioEngine.copyLoop() ?: return
        _uiState.update { it.copy(saving = true, saveMessage = null) }
        scope.launch(Dispatchers.IO) {
            val result = LoopSaver.save(samples, state.sampleRate)
            _uiState.update {
                it.copy(
                    saving = false,
                    saveMessage = result.fold(
                        onSuccess = { path -> "Saved: $path" },
                        onFailure = { e -> "Save failed: ${e.message ?: e.javaClass.simpleName}" },
                    ),
                )
            }
        }
    }

    fun setMonitor(enabled: Boolean) {
        AudioEngine.setMonitor(enabled)
        _uiState.update { it.copy(monitorEnabled = enabled) }
    }

    fun setInputGain(gain: Float) {
        AudioEngine.setInputGain(gain)
        _uiState.update { it.copy(inputGain = gain) }
    }

    fun setOutputGain(gain: Float) {
        AudioEngine.setOutputGain(gain)
        _uiState.update { it.copy(outputGain = gain) }
    }

    fun setBpm(bpm: Int) {
        val clamped = bpm.coerceIn(MIN_BPM, MAX_BPM)
        AudioEngine.setBpm(clamped)
        _uiState.update { it.copy(bpm = clamped) }
    }

    fun setMetronome(enabled: Boolean) {
        AudioEngine.setMetronome(enabled)
        _uiState.update { it.copy(metronomeEnabled = enabled) }
    }

    fun setDrums(enabled: Boolean) {
        AudioEngine.setDrums(enabled)
        _uiState.update { it.copy(drumsEnabled = enabled) }
    }

    /**
     * Live drop-out: silences the drums but leaves the bar clock (and the
     * loop) running, so unmuting re-enters the groove in time. Bound to M.
     */
    fun toggleDrumsMute() {
        val muted = !_uiState.value.drumsMuted
        AudioEngine.setDrumsMuted(muted)
        _uiState.update { it.copy(drumsMuted = muted) }
    }

    /**
     * Switching the meter also moves to that meter's first groove — a groove
     * is written for one bar length, so keeping the old one would either be
     * ignored by the engine or fight the bar clock.
     */
    fun setTimeSignature(index: Int) {
        val state = _uiState.value
        if (index == state.timeSignature) return
        val groove = state.rhythm.groovesFor(index).firstOrNull() ?: state.groove
        AudioEngine.setTimeSignature(index)
        AudioEngine.setGroove(groove)
        _uiState.update { it.copy(timeSignature = index, groove = groove) }
    }

    fun setGroove(index: Int) {
        AudioEngine.setGroove(index)
        _uiState.update { it.copy(groove = index) }
    }

    fun setCountIn(enabled: Boolean) {
        AudioEngine.setCountIn(enabled)
        _uiState.update { it.copy(countInEnabled = enabled) }
    }

    fun setRhythmVolume(volume: Float) {
        AudioEngine.setRhythmVolume(volume)
        _uiState.update { it.copy(rhythmVolume = volume) }
    }

    fun setLoopVolume(volume: Float) {
        AudioEngine.setLoopVolume(volume)
        _uiState.update { it.copy(loopVolume = volume) }
    }

    fun setAutoLoop(enabled: Boolean) {
        val bars = _uiState.value.autoLoopBars
        AudioEngine.setAutoLoopBars(if (enabled) bars else 0)
        _uiState.update { it.copy(autoLoopEnabled = enabled) }
    }

    fun setAutoLoopBars(bars: Int) {
        if (_uiState.value.autoLoopEnabled) {
            AudioEngine.setAutoLoopBars(bars)
        }
        _uiState.update { it.copy(autoLoopBars = bars) }
    }

    /** Call on window close: stops audio and all polling. */
    fun shutdown() {
        scope.cancel()
        AudioEngine.stop()
    }

    companion object {
        private const val POLL_MS = 50L
        private const val DEVICE_POLL_MS = 1500L
        const val MIN_BPM = 40
        const val MAX_BPM = 240
        const val CHAIN_COUNT = 3
    }
}
