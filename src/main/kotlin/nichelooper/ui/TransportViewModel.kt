package nichelooper.ui

import androidx.compose.ui.input.key.Key
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
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
import nichelooper.platform.Platform

/**
 * How the REC button (and its keys) treats an existing loop.
 * - [NORMAL]: REC on a loop records a new take over it (RE-REC).
 * - [ONE_SHOT]: as in Widder — record once, after that REC only starts and stops the loop;
 *   a new take needs CLEAR first. Overdub stays available on its own key. Made for looping an
 *   acoustic guitar through the built-in mic and speakers: the input is only recorded, never
 *   monitored, so nothing can feed back.
 */
enum class LoopMode(val title: String) {
    NORMAL("Normal"),
    ONE_SHOT("Einmal-Aufnahme"),
}

data class TransportUiState(
    val inputDevices: List<String> = emptyList(),
    val outputDevices: List<String> = emptyList(),
    val selectedInput: String? = null,
    val selectedOutput: String? = null,
    val availablePlugins: List<String> = emptyList(),
    val pluginScanDone: Boolean = false,
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
    val loopMuted: Boolean = false,
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
    val keyBindings: KeyBindings = KeyBindings(),
    val capturingKeyFor: KeyAction? = null,
    val textFieldFocused: Boolean = false,
    val loopMode: LoopMode = LoopMode.NORMAL,
    val drumFilterOn: Boolean = true,       // play the drum-filtered take when there is one
    val drumFilterReady: Boolean = false,   // a drum-filtered version of the loop exists
    val commandMode: Boolean = false,    // Ctrl+B pressed, waiting for the command key
    val bpmKeys: BpmKeys = BpmKeys(),
    val bpmKeysDialogOpen: Boolean = false,
    val capturingBpmKey: Boolean = false,
    val bpmKeyDraft: Key? = null,        // key picked in the BPM-Tasten dialog, not yet saved
)

/**
 * Desktop counterpart of the Android TransportViewModel. Devices are chosen
 * by name from the audio backend's enumeration (instead of the USB detector);
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

        // One-time enumeration of installed effect plugins (Audio Units on
        // macOS, VST3 on Linux) for the chain editor.
        scope.launch {
            val plugins = runCatching { AudioEngine.effectPluginNames }
                .getOrDefault(emptyList())
            _uiState.update { it.copy(availablePlugins = plugins, pluginScanDone = true) }
        }

        // Populate the preset menu (bank may be empty on a fresh install).
        refreshPresets()

        // Load user-configured keyboard shortcuts.
        scope.launch(Dispatchers.IO) {
            val loaded = KeyBindingsStore.load()
            val bpmKeys = BpmKeysStore.load()
            _uiState.update { it.copy(keyBindings = loaded, bpmKeys = bpmKeys) }
        }

        // Poll the engine for playhead/state; also catches disconnects
        // reported from the native notification callback.
        scope.launch {
            while (isActive) {
                delay(POLL_MS)
                if (!_uiState.value.engineRunning) continue
                if (AudioEngine.isDisconnected || !AudioEngine.isRunning) {
                    stopEngine("Audio device stopped (unplugged or reconfigured). Press Start to restart.")
                } else {
                    val previous = _uiState.value.looperState
                    val current = AudioEngine.looperState
                    if (previous == LooperState.RECORDING &&
                        (current == LooperState.PLAYING || current == LooperState.STOPPED)
                    ) {
                        onTakeClosed()
                    }
                    val inPeak = AudioEngine.readInputPeak()
                    val fxPeak = AudioEngine.readFxPeak()
                    val outPeak = AudioEngine.readOutputPeak()
                    _uiState.update {
                        it.copy(
                            looperState = current,
                            // A new take, Clear or Load drops the filtered version.
                            drumFilterReady = it.drumFilterReady && AudioEngine.isDrumFilterReady,
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
            check(AudioEngine.refreshDevices()) { "audio context failed" }
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
                    applyLoopMode(s)
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
                    AudioEngine.setLoopVolume(if (s.loopMuted) 0f else s.loopVolume)
                    AudioEngine.setDrumFilter(s.drumFilterOn)
                    AudioEngine.setAutoLoopBars(if (s.autoLoopEnabled) s.autoLoopBars else 0)
                    _uiState.update {
                        it.copy(
                            engineRunning = true,
                            drumFilterReady = false,
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
                                START_FAILED_HINT,
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
                    drumFilterReady = false,
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

    /**
     * Adds the plugin at [path] (Linux: a .vst3 folder anywhere on disk) to the
     * active chain — for plugins outside the standard VST3 folders.
     */
    fun addPluginFromPath(path: String) {
        val chain = _uiState.value.activeChain
        scope.launch {
            val ok = AudioEngine.chainAddPluginFromPath(chain, path)
            _uiState.update {
                it.copy(
                    chains = readChains(),
                    saveMessage = if (ok) null else "Plugin failed to load: $path",
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

    /**
     * Combined Stop / Record action (bound to Spacebar by default):
     * - When EMPTY or STOPPED: starts recording (or count-in).
     * - When RECORDING: stops recording (with manual loop length, sets the loop
     *   so playback starts immediately; with auto-loop, stops playback).
     * - When COUNT_IN, PLAYING, or OVERDUBBING: stops the loop / cancels count-in.
     */
    fun recordOrStop() {
        val s = _uiState.value
        if (!s.engineRunning) return
        when (s.looperState) {
            LooperState.EMPTY, LooperState.STOPPED -> record()
            LooperState.RECORDING -> if (s.autoLoopEnabled) stopLoop() else record()
            LooperState.COUNT_IN, LooperState.PLAYING, LooperState.OVERDUBBING -> stopLoop()
        }
    }

    /** The REC button and the Record key: depends on [LoopMode]. */
    fun recordButton() {
        if (_uiState.value.loopMode == LoopMode.ONE_SHOT) oneShotStep(::record) else record()
    }

    /**
     * One-shot step (REC button, R and Space in [LoopMode.ONE_SHOT]). Until there is a loop it
     * does exactly what [normal] does in the normal mode — count-in, auto-loop and the way a
     * recording is closed stay the same. With a loop: PLAYING → stop, STOPPED → play; it never
     * records over it.
     */
    private fun oneShotStep(normal: () -> Unit) {
        val s = _uiState.value
        if (!s.engineRunning) return
        when (s.looperState) {
            LooperState.EMPTY, LooperState.COUNT_IN, LooperState.RECORDING -> normal()
            LooperState.STOPPED -> play()
            LooperState.PLAYING, LooperState.OVERDUBBING -> stopLoop()
        }
    }

    /**
     * In one-shot mode the mic also heard the click and drums from the speakers. Build the
     * drum-filtered version of the fresh take; [toggleDrumFilter] (F) then switches between it
     * and the original. Runs while the loop already plays.
     */
    private fun onTakeClosed() {
        val s = _uiState.value
        if (s.loopMode != LoopMode.ONE_SHOT) return
        if (!s.metronomeEnabled && !s.drumsEnabled && !s.countInEnabled) return
        scope.launch {
            val ok = AudioEngine.prepareDrumFilter()
            _uiState.update {
                it.copy(
                    drumFilterReady = ok,
                    saveMessage = if (ok) {
                        "Drum-Filter bereit — ${it.keyBindings.shortLabelFor(KeyAction.DRUM_FILTER) ?: "Knopf"} " +
                            "schaltet die Drums im Loop an/aus"
                    } else {
                        "Drum-Filter: keine Drums in der Aufnahme gefunden"
                    },
                )
            }
        }
    }

    /** Drums in the loop on/off: plays the original take or the drum-filtered one. */
    fun toggleDrumFilter() {
        val on = !_uiState.value.drumFilterOn
        AudioEngine.setDrumFilter(on)
        _uiState.update { it.copy(drumFilterOn = on) }
    }

    fun setLoopMode(mode: LoopMode) {
        _uiState.update { it.copy(loopMode = mode) }
        applyLoopMode(_uiState.value)
    }

    /** Monitoring and the post-close fold both feed the mic back to the speakers — off in one-shot mode. */
    private fun applyLoopMode(s: TransportUiState) {
        val room = s.loopMode == LoopMode.ONE_SHOT
        AudioEngine.setMonitor(s.monitorEnabled && !room)
        AudioEngine.setPostCloseFold(!room)
    }

    fun toggleLoopMode() = setLoopMode(
        if (_uiState.value.loopMode == LoopMode.NORMAL) LoopMode.ONE_SHOT else LoopMode.NORMAL,
    )

    // ---- Command mode (Ctrl+B, then a command key) ----

    private var commandTimeout: Job? = null

    /** Ctrl+B: the next key is a command. Lapses after a few seconds so a forgotten prefix cannot eat a later key. */
    fun enterCommandMode() {
        commandTimeout?.cancel()
        _uiState.update { it.copy(commandMode = true) }
        commandTimeout = scope.launch {
            delay(COMMAND_TIMEOUT_MS)
            _uiState.update { it.copy(commandMode = false) }
        }
    }

    fun cancelCommandMode() {
        commandTimeout?.cancel()
        _uiState.update { it.copy(commandMode = false) }
    }

    /** Runs the command for [key] after Ctrl+B. Unknown keys just end command mode. */
    fun runCommand(key: Key) {
        cancelCommandMode()
        when (key) {
            Key.L -> toggleLoopMode()
            Key.V -> openBpmKeysDialog()
            else -> {}
        }
    }

    // ---- BPM keys (Shift + key = tempo) ----

    /** Shift + [key]: jumps to the tempo stored for it. Returns false if the key has none. */
    fun applyBpmKey(key: Key): Boolean {
        val s = _uiState.value
        val bpm = s.bpmKeys.bpmFor(key) ?: return false
        if (s.engineRunning) setBpm(bpm)
        return true
    }

    fun openBpmKeysDialog() {
        _uiState.update { it.copy(bpmKeysDialogOpen = true, capturingBpmKey = false, bpmKeyDraft = null) }
    }

    fun closeBpmKeysDialog() {
        _uiState.update { it.copy(bpmKeysDialogOpen = false, capturingBpmKey = false, bpmKeyDraft = null) }
    }

    fun setCapturingBpmKey(capturing: Boolean) {
        _uiState.update { it.copy(capturingBpmKey = capturing) }
    }

    fun pickBpmKey(key: Key) {
        _uiState.update { it.copy(bpmKeyDraft = key, capturingBpmKey = false) }
    }

    fun putBpmKey(key: Key, bpm: Int) {
        val updated = _uiState.value.bpmKeys.with(key, bpm)
        _uiState.update { it.copy(bpmKeys = updated, bpmKeyDraft = null) }
        scope.launch(Dispatchers.IO) { BpmKeysStore.save(updated) }
    }

    fun removeBpmKey(key: Key) {
        val updated = _uiState.value.bpmKeys.without(key)
        _uiState.update { it.copy(bpmKeys = updated) }
        scope.launch(Dispatchers.IO) { BpmKeysStore.save(updated) }
    }

    fun playOrStop() {
        val s = _uiState.value
        if (!s.engineRunning) return
        when (s.looperState) {
            LooperState.STOPPED -> if (s.loopLengthFrames > 0) play()
            LooperState.RECORDING, LooperState.PLAYING, LooperState.OVERDUBBING,
            LooperState.COUNT_IN -> stopLoop()
            LooperState.EMPTY -> {}
        }
    }

    fun setTextFieldFocused(focused: Boolean) {
        _uiState.update { it.copy(textFieldFocused = focused) }
    }

    fun setCapturingKeyFor(action: KeyAction?) {
        _uiState.update { it.copy(capturingKeyFor = action) }
    }

    fun assignKey(action: KeyAction, key: Key?) {
        val updated = _uiState.value.keyBindings.withKey(action, key)
        _uiState.update { it.copy(keyBindings = updated, capturingKeyFor = null) }
        scope.launch(Dispatchers.IO) { KeyBindingsStore.save(updated) }
    }

    fun resetKeyBindings() {
        val defaults = KeyBindings()
        _uiState.update { it.copy(keyBindings = defaults, capturingKeyFor = null) }
        scope.launch(Dispatchers.IO) { KeyBindingsStore.save(defaults) }
    }

    fun performKeyAction(action: KeyAction) {
        val s = _uiState.value
        when (action) {
            KeyAction.REC_STOP -> if (s.loopMode == LoopMode.ONE_SHOT) oneShotStep(::recordOrStop) else recordOrStop()
            KeyAction.RECORD -> if (s.engineRunning && s.looperState != LooperState.COUNT_IN) recordButton()
            KeyAction.STOP -> if (s.engineRunning) stopLoop()
            KeyAction.PLAY_STOP -> playOrStop()
            KeyAction.OVERDUB -> if (s.engineRunning && s.loopLengthFrames > 0) toggleOverdub()
            KeyAction.CLEAR -> if (s.engineRunning) clear()
            KeyAction.SAVE_LOOP -> saveLoop()
            KeyAction.CHAIN_1 -> setActiveChain(0)
            KeyAction.CHAIN_2 -> setActiveChain(1)
            KeyAction.CHAIN_3 -> setActiveChain(2)
            KeyAction.DRUMS_MUTE -> toggleDrumsMute()
            KeyAction.LOOP_MUTE -> toggleLoopMute()
            KeyAction.DRUM_FILTER -> toggleDrumFilter()
            KeyAction.DRUMS_TOGGLE -> if (s.engineRunning) setDrums(!s.drumsEnabled)
            KeyAction.METRONOME_TOGGLE -> if (s.engineRunning) setMetronome(!s.metronomeEnabled)
            KeyAction.COUNT_IN_TOGGLE -> if (s.engineRunning) setCountIn(!s.countInEnabled)
            KeyAction.AUTO_LOOP_TOGGLE -> if (s.engineRunning) setAutoLoop(!s.autoLoopEnabled)
            KeyAction.MONITOR_TOGGLE ->
                if (s.engineRunning && s.loopMode == LoopMode.NORMAL) setMonitor(!s.monitorEnabled)
            KeyAction.BPM_DOWN -> if (s.engineRunning) setBpm(s.bpm - 1)
            KeyAction.BPM_UP -> if (s.engineRunning) setBpm(s.bpm + 1)
        }
    }

    /** Snapshots the loop and writes it to NicheLooper/ in the music folder — audio keeps running. */
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

    /** The normal mode's monitor setting; one-shot mode keeps monitoring off regardless (see [applyLoopMode]). */
    fun setMonitor(enabled: Boolean) {
        _uiState.update { it.copy(monitorEnabled = enabled) }
        applyLoopMode(_uiState.value)
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
        if (!_uiState.value.loopMuted) AudioEngine.setLoopVolume(volume)
        _uiState.update { it.copy(loopVolume = volume) }
    }

    /**
     * Silences the loop but keeps it running (and the drums locked to it), so unmuting lands
     * back in time — the loop's counterpart to [toggleDrumsMute]. Bound to N.
     */
    fun toggleLoopMute() {
        val s = _uiState.value
        val muted = !s.loopMuted
        AudioEngine.setLoopVolume(if (muted) 0f else s.loopVolume)
        _uiState.update { it.copy(loopMuted = muted) }
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
        private const val COMMAND_TIMEOUT_MS = 4000L
        const val MIN_BPM = 40
        const val MAX_BPM = 240
        const val CHAIN_COUNT = 3

        private val START_FAILED_HINT =
            if (Platform.isMac) {
                "Check the microphone permission in System Settings."
            } else {
                "Check that no other program is holding the devices; the environment variable " +
                    "NICHELOOPER_AUDIO_BACKEND (pulseaudio, alsa or jack) picks the sound system."
            }
    }
}
