package nichelooper.ui

import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
import androidx.compose.material3.ButtonDefaults
import androidx.compose.material3.Card
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.DropdownMenu
import androidx.compose.material3.DropdownMenuItem
import androidx.compose.material3.FilterChip
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.HorizontalDivider
import androidx.compose.material3.Slider
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import java.util.Locale
import nichelooper.audio.LooperState
import nichelooper.audio.SavedLoop

private val RecordRed = Color(0xFFE53935)
private val OverdubOrange = Color(0xFFFB8C00)
private val PlayGreen = Color(0xFF43A047)

private val TimeSignatureNames = listOf("4/4", "3/4", "6/8")
private val AutoLoopBarChoices = listOf(4, 6, 8)
private val ChainNames = listOf("A", "S", "D")

private fun framesPerBeat(sampleRate: Int, bpm: Int): Int =
    if (sampleRate > 0 && bpm > 0) sampleRate * 60 / bpm else 0

/** Beats per bar for the time-signature index (must match RhythmSection). */
private fun beatsPerBar(timeSignature: Int): Int = when (timeSignature) {
    1 -> 3   // 3/4
    2 -> 6   // 6/8 (BPM counts eighths)
    else -> 4
}

@Composable
fun TransportScreen(viewModel: TransportViewModel) {
    val state by viewModel.uiState.collectAsState()

    Column(
        modifier = Modifier
            .fillMaxSize()
            .verticalScroll(rememberScrollState())
            .padding(16.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(16.dp),
    ) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                text = "NicheLooper",
                style = MaterialTheme.typography.headlineSmall,
                fontWeight = FontWeight.Bold,
            )
            PresetMenu(
                state = state,
                onSave = viewModel::savePreset,
                onLoad = viewModel::loadPreset,
                onDelete = viewModel::deletePreset,
                onRefresh = viewModel::refreshPresets,
            )
        }

        ErrorCard(state = state, onRetry = viewModel::startEngine)

        DeviceCard(
            state = state,
            onSelectInput = viewModel::selectInputDevice,
            onSelectOutput = viewModel::selectOutputDevice,
            onStart = viewModel::startEngine,
            onStop = viewModel::stopEngineByUser,
        )

        ChainsCard(
            state = state,
            onSelectChain = viewModel::setActiveChain,
            onAddPlugin = viewModel::addPluginToChain,
            onRemovePlugin = viewModel::removePluginFromChain,
            onMovePluginUp = viewModel::movePluginUp,
            onOpenEditor = viewModel::openPluginEditor,
        )

        LoopIndicator(state = state)

        RecordButton(
            state = state,
            onClick = viewModel::record,
        )

        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            val hasLoop = state.loopLengthFrames > 0
            val enabled = state.engineRunning

            OutlinedButton(
                onClick = viewModel::play,
                enabled = enabled && hasLoop && state.looperState == LooperState.STOPPED,
            ) { Text("PLAY") }

            OutlinedButton(
                onClick = viewModel::stopLoop,
                enabled = enabled && state.looperState in setOf(
                    LooperState.RECORDING, LooperState.PLAYING, LooperState.OVERDUBBING,
                    LooperState.COUNT_IN,
                ),
            ) { Text("STOP") }

            Button(
                onClick = viewModel::toggleOverdub,
                enabled = enabled && hasLoop,
                colors = ButtonDefaults.buttonColors(
                    containerColor = if (state.looperState == LooperState.OVERDUBBING) {
                        OverdubOrange
                    } else {
                        MaterialTheme.colorScheme.secondaryContainer
                    },
                    contentColor = if (state.looperState == LooperState.OVERDUBBING) {
                        Color.White
                    } else {
                        MaterialTheme.colorScheme.onSecondaryContainer
                    },
                ),
            ) { Text("OVERDUB") }
        }

        var showLoopPicker by remember { mutableStateOf(false) }

        Row(horizontalArrangement = Arrangement.spacedBy(12.dp)) {
            OutlinedButton(
                onClick = viewModel::clear,
                enabled = state.engineRunning &&
                    (state.loopLengthFrames > 0 || state.looperState == LooperState.RECORDING),
            ) { Text("CLEAR") }

            Button(
                onClick = viewModel::saveLoop,
                enabled = state.loopLengthFrames > 0 && !state.saving,
            ) { Text(if (state.saving) "SAVING…" else "SAVE") }

            OutlinedButton(
                onClick = {
                    viewModel.refreshSavedLoops()
                    showLoopPicker = true
                },
                enabled = state.engineRunning && !state.loadingLoop,
            ) { Text(if (state.loadingLoop) "LOADING…" else "LOAD") }
        }

        if (showLoopPicker) {
            LoopPickerDialog(
                loops = state.savedLoops,
                onPick = { loop ->
                    showLoopPicker = false
                    viewModel.loadSavedLoop(loop)
                },
                onDismiss = { showLoopPicker = false },
            )
        }

        state.saveMessage?.let { message ->
            Text(
                text = message,
                style = MaterialTheme.typography.bodySmall,
                color = if (message.startsWith("Saved") || message.startsWith("Loaded") ||
                    message.startsWith("Preset gespeichert") || message.startsWith("Preset geladen")) {
                    PlayGreen
                } else {
                    MaterialTheme.colorScheme.error
                },
                textAlign = TextAlign.Center,
            )
        }

        RhythmCard(
            state = state,
            onBpm = viewModel::setBpm,
            onMetronome = viewModel::setMetronome,
            onCountIn = viewModel::setCountIn,
            onDrums = viewModel::setDrums,
            onTimeSignature = viewModel::setTimeSignature,
            onAutoLoop = viewModel::setAutoLoop,
            onAutoLoopBars = viewModel::setAutoLoopBars,
            onVolume = viewModel::setRhythmVolume,
        )

        Card(modifier = Modifier.fillMaxWidth()) {
            Column(
                modifier = Modifier.padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                LevelMeter(label = "In", peak = state.inputPeak)
                LevelMeter(label = "FX", peak = state.fxPeak)
                LevelMeter(label = "Out", peak = state.outputPeak)

                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text("Monitor input", style = MaterialTheme.typography.bodyLarge)
                    Switch(
                        checked = state.monitorEnabled,
                        onCheckedChange = viewModel::setMonitor,
                        enabled = state.engineRunning,
                    )
                }

                GainSlider(
                    label = "Input gain",
                    value = state.inputGain,
                    enabled = state.engineRunning,
                    onValueChange = viewModel::setInputGain,
                )
                GainSlider(
                    label = "Output gain",
                    value = state.outputGain,
                    enabled = state.engineRunning,
                    onValueChange = viewModel::setOutputGain,
                )
                GainSlider(
                    label = "Loop volume",
                    value = state.loopVolume,
                    enabled = state.engineRunning,
                    onValueChange = viewModel::setLoopVolume,
                    valueRange = 0f..2f,
                )
            }
        }
    }
}

@Composable
private fun ErrorCard(state: TransportUiState, onRetry: () -> Unit) {
    val error = state.errorMessage ?: return
    Card(
        modifier = Modifier.fillMaxWidth(),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.errorContainer,
        ),
    ) {
        Column(modifier = Modifier.padding(16.dp)) {
            Text(
                text = error,
                color = MaterialTheme.colorScheme.onErrorContainer,
                style = MaterialTheme.typography.bodyMedium,
            )
            if (!state.engineRunning) {
                TextButton(onClick = onRetry) { Text("Retry") }
            }
        }
    }
}

@Composable
private fun DeviceCard(
    state: TransportUiState,
    onSelectInput: (String) -> Unit,
    onSelectOutput: (String) -> Unit,
    onStart: () -> Unit,
    onStop: () -> Unit,
) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(12.dp),
            ) {
                Spacer(
                    modifier = Modifier
                        .size(12.dp)
                        .background(
                            color = when {
                                state.engineRunning -> PlayGreen
                                state.inputDevices.isNotEmpty() -> OverdubOrange
                                else -> MaterialTheme.colorScheme.outline
                            },
                            shape = CircleShape,
                        ),
                )
                Text(
                    text = if (state.engineRunning) {
                        "${state.sampleRate} Hz · burst ${state.framesPerBurst} frames"
                    } else if (state.inputDevices.isEmpty()) {
                        "Looking for audio devices…"
                    } else {
                        "Engine stopped"
                    },
                    style = MaterialTheme.typography.bodyLarge,
                    fontWeight = FontWeight.Medium,
                )
            }

            DeviceSelector(
                label = "Input",
                devices = state.inputDevices,
                selected = state.selectedInput,
                onSelect = onSelectInput,
            )
            DeviceSelector(
                label = "Output",
                devices = state.outputDevices,
                selected = state.selectedOutput,
                onSelect = onSelectOutput,
            )

            if (state.engineRunning) {
                OutlinedButton(
                    onClick = onStop,
                    modifier = Modifier.fillMaxWidth(),
                ) { Text("STOP ENGINE") }
            } else {
                Button(
                    onClick = onStart,
                    enabled = state.inputDevices.isNotEmpty() && state.outputDevices.isNotEmpty(),
                    modifier = Modifier.fillMaxWidth(),
                ) { Text("START ENGINE") }
            }
        }
    }
}

@Composable
private fun DeviceSelector(
    label: String,
    devices: List<String>,
    selected: String?,
    onSelect: (String) -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(
            label,
            style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.width(56.dp),
        )
        Box(modifier = Modifier.weight(1f)) {
            OutlinedButton(
                onClick = { expanded = true },
                enabled = devices.isNotEmpty(),
                modifier = Modifier.fillMaxWidth(),
            ) {
                Text(
                    selected ?: "—",
                    maxLines = 1,
                    overflow = TextOverflow.Ellipsis,
                )
            }
            DropdownMenu(
                expanded = expanded,
                onDismissRequest = { expanded = false },
            ) {
                devices.forEach { name ->
                    DropdownMenuItem(
                        text = { Text(name) },
                        onClick = {
                            expanded = false
                            onSelect(name)
                        },
                    )
                }
            }
        }
    }
}

@Composable
private fun ChainsCard(
    state: TransportUiState,
    onSelectChain: (Int) -> Unit,
    onAddPlugin: (Int) -> Unit,
    onRemovePlugin: (Int) -> Unit,
    onMovePluginUp: (Int) -> Unit,
    onOpenEditor: (Int) -> Unit,
) {
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    "Plugin chain",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                )
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    ChainNames.forEachIndexed { index, name ->
                        FilterChip(
                            selected = state.activeChain == index,
                            onClick = { onSelectChain(index) },
                            label = { Text(name) },
                        )
                    }
                }
            }
            Text(
                "Keys A · S · D switch the live chain — the loop keeps its recorded sound.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )

            val plugins = state.chains.getOrNull(state.activeChain).orEmpty()
            if (plugins.isEmpty()) {
                Text(
                    "Empty chain — dry signal.",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            plugins.forEachIndexed { slot, name ->
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(4.dp),
                ) {
                    Text(
                        "${slot + 1}.",
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.width(20.dp),
                    )
                    Text(
                        name.substringAfter(": "),
                        style = MaterialTheme.typography.bodyMedium,
                        maxLines = 1,
                        overflow = TextOverflow.Ellipsis,
                        modifier = Modifier
                            .weight(1f)
                            .clickable { onOpenEditor(slot) },
                    )
                    TextButton(
                        onClick = { onMovePluginUp(slot) },
                        enabled = slot > 0,
                        contentPadding = PaddingValues(horizontal = 8.dp),
                    ) { Text("↑") }
                    TextButton(
                        onClick = { onOpenEditor(slot) },
                        contentPadding = PaddingValues(horizontal = 8.dp),
                    ) { Text("EDIT") }
                    TextButton(
                        onClick = { onRemovePlugin(slot) },
                        contentPadding = PaddingValues(horizontal = 8.dp),
                    ) { Text("✕") }
                }
            }

            var expanded by remember { mutableStateOf(false) }
            Box(modifier = Modifier.fillMaxWidth()) {
                OutlinedButton(
                    onClick = { expanded = true },
                    enabled = state.availablePlugins.isNotEmpty(),
                    modifier = Modifier.fillMaxWidth(),
                ) { Text("+ ADD PLUGIN") }
                DropdownMenu(
                    expanded = expanded,
                    onDismissRequest = { expanded = false },
                    modifier = Modifier.heightIn(max = 400.dp),
                ) {
                    state.availablePlugins.forEachIndexed { index, name ->
                        DropdownMenuItem(
                            text = { Text(name) },
                            onClick = {
                                expanded = false
                                onAddPlugin(index)
                            },
                        )
                    }
                }
            }
        }
    }
}

@Composable
private fun LoopIndicator(state: TransportUiState) {
    val looper = state.looperState
    val rate = state.sampleRate

    val (label, color) = when (looper) {
        LooperState.EMPTY -> "EMPTY" to MaterialTheme.colorScheme.onSurfaceVariant
        LooperState.RECORDING -> "RECORDING" to RecordRed
        LooperState.PLAYING -> "PLAYING" to PlayGreen
        LooperState.OVERDUBBING -> "OVERDUBBING" to OverdubOrange
        LooperState.STOPPED -> "STOPPED" to MaterialTheme.colorScheme.onSurfaceVariant
        LooperState.COUNT_IN -> "COUNT-IN" to RecordRed
    }

    val beatFrames = framesPerBeat(rate, state.bpm)
    val beats = beatsPerBar(state.timeSignature)
    val barFrames = beatFrames * beats
    val autoLoopFrames =
        if (state.autoLoopEnabled && barFrames > 0) state.autoLoopBars * barFrames else 0

    val timeText = when {
        rate <= 0 -> "—"
        looper == LooperState.COUNT_IN && beatFrames > 0 ->
            "Beat ${(state.positionFrames / beatFrames + 1).coerceIn(1, beats)} / $beats"
        looper == LooperState.RECORDING && autoLoopFrames > 0 ->
            "Bar ${(state.positionFrames / barFrames + 1).coerceIn(1, state.autoLoopBars)} / ${state.autoLoopBars}"
        looper == LooperState.RECORDING ->
            formatSeconds(state.positionFrames, rate)
        state.loopLengthFrames > 0 ->
            "${formatSeconds(state.positionFrames, rate)} / ${formatSeconds(state.loopLengthFrames, rate)}"
        else -> "—"
    }

    val progress = when {
        looper == LooperState.COUNT_IN && barFrames > 0 ->
            state.positionFrames.toFloat() / barFrames
        looper == LooperState.RECORDING && autoLoopFrames > 0 ->
            state.positionFrames.toFloat() / autoLoopFrames
        looper == LooperState.RECORDING && state.maxLoopFrames > 0 ->
            state.positionFrames.toFloat() / state.maxLoopFrames
        state.loopLengthFrames > 0 ->
            state.positionFrames.toFloat() / state.loopLengthFrames
        else -> 0f
    }

    Column(
        modifier = Modifier.fillMaxWidth(),
        horizontalAlignment = Alignment.CenterHorizontally,
        verticalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(
            text = label,
            color = color,
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.Bold,
        )
        Text(
            text = timeText,
            style = MaterialTheme.typography.headlineMedium,
            fontWeight = FontWeight.Light,
        )
        LinearProgressIndicator(
            progress = { progress.coerceIn(0f, 1f) },
            modifier = Modifier
                .fillMaxWidth()
                .height(8.dp),
            color = color,
        )
    }
}

@Composable
private fun RecordButton(state: TransportUiState, onClick: () -> Unit) {
    val counting = state.looperState == LooperState.COUNT_IN
    val label = when (state.looperState) {
        LooperState.RECORDING -> "SET\nLOOP"
        LooperState.EMPTY -> "REC"
        LooperState.COUNT_IN -> {
            val beatFrames = framesPerBeat(state.sampleRate, state.bpm)
            if (beatFrames > 0) {
                "${(state.positionFrames / beatFrames + 1).coerceIn(1, beatsPerBar(state.timeSignature))}"
            } else {
                "…"
            }
        }
        else -> "RE-\nREC"
    }
    Button(
        onClick = onClick,
        enabled = state.engineRunning && !counting,
        shape = CircleShape,
        modifier = Modifier.size(120.dp),
        colors = ButtonDefaults.buttonColors(
            containerColor = RecordRed,
            contentColor = Color.White,
            disabledContainerColor = if (counting) RecordRed else ButtonDefaults.buttonColors().disabledContainerColor,
            disabledContentColor = if (counting) Color.White else ButtonDefaults.buttonColors().disabledContentColor,
        ),
    ) {
        Text(
            text = label,
            textAlign = TextAlign.Center,
            fontSize = if (counting) 44.sp else 20.sp,
            fontWeight = FontWeight.Bold,
        )
    }
}

@Composable
private fun RhythmCard(
    state: TransportUiState,
    onBpm: (Int) -> Unit,
    onMetronome: (Boolean) -> Unit,
    onCountIn: (Boolean) -> Unit,
    onDrums: (Boolean) -> Unit,
    onTimeSignature: (Int) -> Unit,
    onAutoLoop: (Boolean) -> Unit,
    onAutoLoopBars: (Int) -> Unit,
    onVolume: (Float) -> Unit,
) {
    val enabled = state.engineRunning
    Card(modifier = Modifier.fillMaxWidth()) {
        Column(
            modifier = Modifier.padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
        ) {
            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    "Rhythm",
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold,
                )
                Text(
                    "${state.bpm} BPM",
                    style = MaterialTheme.typography.titleMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp),
            ) {
                OutlinedButton(
                    onClick = { onBpm(state.bpm - 1) },
                    enabled = enabled,
                    shape = CircleShape,
                    contentPadding = PaddingValues(0.dp),
                    modifier = Modifier.size(36.dp),
                ) { Text("−") }
                Slider(
                    value = state.bpm.toFloat(),
                    onValueChange = { onBpm(it.toInt()) },
                    valueRange = TransportViewModel.MIN_BPM.toFloat()..TransportViewModel.MAX_BPM.toFloat(),
                    enabled = enabled,
                    modifier = Modifier.weight(1f),
                )
                OutlinedButton(
                    onClick = { onBpm(state.bpm + 1) },
                    enabled = enabled,
                    shape = CircleShape,
                    contentPadding = PaddingValues(0.dp),
                    modifier = Modifier.size(36.dp),
                ) { Text("+") }
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.spacedBy(8.dp),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text(
                    "Time",
                    style = MaterialTheme.typography.bodyLarge,
                    modifier = Modifier.weight(1f),
                )
                TimeSignatureNames.forEachIndexed { index, name ->
                    FilterChip(
                        selected = state.timeSignature == index,
                        onClick = { onTimeSignature(index) },
                        label = { Text(name) },
                        enabled = enabled,
                    )
                }
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text("Metronome", style = MaterialTheme.typography.bodyLarge)
                Switch(
                    checked = state.metronomeEnabled,
                    onCheckedChange = onMetronome,
                    enabled = enabled,
                )
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text("Count-in (2 bars)", style = MaterialTheme.typography.bodyLarge)
                Switch(
                    checked = state.countInEnabled,
                    onCheckedChange = onCountIn,
                    enabled = enabled,
                )
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text("Drums", style = MaterialTheme.typography.bodyLarge)
                Switch(
                    checked = state.drumsEnabled,
                    onCheckedChange = onDrums,
                    enabled = enabled,
                )
            }

            Row(
                modifier = Modifier.fillMaxWidth(),
                horizontalArrangement = Arrangement.SpaceBetween,
                verticalAlignment = Alignment.CenterVertically,
            ) {
                Text("Auto-loop", style = MaterialTheme.typography.bodyLarge)
                Switch(
                    checked = state.autoLoopEnabled,
                    onCheckedChange = onAutoLoop,
                    enabled = enabled,
                )
            }

            if (state.autoLoopEnabled) {
                Row(
                    modifier = Modifier.fillMaxWidth(),
                    horizontalArrangement = Arrangement.spacedBy(8.dp),
                    verticalAlignment = Alignment.CenterVertically,
                ) {
                    Text(
                        "Bars",
                        style = MaterialTheme.typography.bodyMedium,
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                        modifier = Modifier.weight(1f),
                    )
                    AutoLoopBarChoices.forEach { bars ->
                        FilterChip(
                            selected = state.autoLoopBars == bars,
                            onClick = { onAutoLoopBars(bars) },
                            label = { Text("$bars") },
                            enabled = enabled,
                        )
                    }
                }
            }

            GainSlider(
                label = "Drums & click volume",
                value = state.rhythmVolume,
                enabled = enabled,
                onValueChange = onVolume,
                valueRange = 0f..2f,
            )
        }
    }
}

@Composable
private fun LoopPickerDialog(
    loops: List<SavedLoop>,
    onPick: (SavedLoop) -> Unit,
    onDismiss: () -> Unit,
) {
    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Saved loops") },
        text = {
            if (loops.isEmpty()) {
                Text("No saved loops yet. Record one and press SAVE.")
            } else {
                LazyColumn(modifier = Modifier.heightIn(max = 400.dp)) {
                    items(loops) { loop ->
                        Column(
                            modifier = Modifier
                                .fillMaxWidth()
                                .clickable { onPick(loop) }
                                .padding(vertical = 10.dp),
                        ) {
                            Text(loop.name, style = MaterialTheme.typography.bodyLarge)
                            if (loop.durationMs > 0) {
                                Text(
                                    String.format(
                                        Locale.US, "%.1f s", loop.durationMs / 1000.0,
                                    ),
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                        }
                    }
                }
            }
        },
        confirmButton = {},
        dismissButton = {
            TextButton(onClick = onDismiss) { Text("Cancel") }
        },
    )
}

@Composable
private fun LevelMeter(label: String, peak: Float) {
    val db = 20.0 * kotlin.math.log10(peak.coerceAtLeast(1e-4f).toDouble())
    val progress = ((db + 60.0) / 60.0).toFloat().coerceIn(0f, 1f)
    val color = when {
        db >= -1.0 -> RecordRed          // clipping / hard limiter engaged
        db >= -9.0 -> OverdubOrange
        else -> PlayGreen
    }
    Row(
        modifier = Modifier.fillMaxWidth(),
        verticalAlignment = Alignment.CenterVertically,
        horizontalArrangement = Arrangement.spacedBy(8.dp),
    ) {
        Text(
            label,
            style = MaterialTheme.typography.bodySmall,
            modifier = Modifier.width(28.dp),
        )
        LinearProgressIndicator(
            progress = { progress },
            modifier = Modifier
                .weight(1f)
                .height(10.dp),
            color = color,
        )
        Text(
            if (db <= -79.9) "-inf dB" else String.format(Locale.US, "%.1f dB", db),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
            modifier = Modifier.width(64.dp),
            textAlign = TextAlign.End,
        )
    }
}

@Composable
private fun GainSlider(
    label: String,
    value: Float,
    enabled: Boolean,
    onValueChange: (Float) -> Unit,
    valueRange: ClosedFloatingPointRange<Float> = 0f..4f,
) {
    Column {
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween,
        ) {
            Text(label, style = MaterialTheme.typography.bodyMedium)
            Text(
                String.format(Locale.US, "%.0f%%", value * 100),
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
        }
        Slider(
            value = value,
            onValueChange = onValueChange,
            valueRange = valueRange,
            enabled = enabled,
        )
    }
}

@Composable
private fun PresetMenu(
    state: TransportUiState,
    onSave: (String) -> Unit,
    onLoad: (String) -> Unit,
    onDelete: (String) -> Unit,
    onRefresh: () -> Unit,
) {
    var expanded by remember { mutableStateOf(false) }
    var showSaveDialog by remember { mutableStateOf(false) }
    var name by remember { mutableStateOf("") }

    Box {
        OutlinedButton(onClick = { onRefresh(); expanded = true }) {
            Text("Presets ▾")
        }
        DropdownMenu(
            expanded = expanded,
            onDismissRequest = { expanded = false },
            modifier = Modifier.widthIn(max = 260.dp),
        ) {
            DropdownMenuItem(
                text = { Text("Als Preset speichern…") },
                onClick = { expanded = false; showSaveDialog = true },
            )
            if (state.presets.isEmpty()) {
                DropdownMenuItem(
                    text = {
                        Text(
                            "Noch keine Presets",
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    },
                    onClick = {},
                    enabled = false,
                )
            } else {
                HorizontalDivider()
                state.presets.forEach { preset ->
                    DropdownMenuItem(
                        text = {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Text(
                                    preset,
                                    modifier = Modifier.weight(1f),
                                    maxLines = 1,
                                    overflow = TextOverflow.Ellipsis,
                                )
                                // ✎ = aktuellen Stand in dieses Preset
                                // ueberschreiben (in-place bearbeiten).
                                Text(
                                    "✎",
                                    modifier = Modifier
                                        .padding(end = 8.dp)
                                        .clickable {
                                            onSave(preset)
                                            onRefresh()
                                            expanded = false
                                        },
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                                Text(
                                    "✕",
                                    modifier = Modifier.clickable {
                                        onDelete(preset)
                                        onRefresh()
                                        expanded = false
                                    },
                                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                                )
                            }
                        },
                        onClick = { expanded = false; onLoad(preset) },
                    )
                }
            }
        }
    }

    if (showSaveDialog) {
        AlertDialog(
            onDismissRequest = { showSaveDialog = false; name = "" },
            title = { Text("Preset speichern") },
            text = {
                OutlinedTextField(
                    value = name,
                    onValueChange = { name = it },
                    singleLine = true,
                    label = { Text("Name") },
                )
            },
            confirmButton = {
                TextButton(
                    onClick = {
                        if (name.isNotBlank()) {
                            onSave(name.trim())
                            showSaveDialog = false
                            name = ""
                        }
                    },
                ) { Text("Speichern") }
            },
            dismissButton = {
                TextButton(onClick = { showSaveDialog = false; name = "" }) { Text("Abbrechen") }
            },
        )
    }
}

private fun formatSeconds(frames: Int, sampleRate: Int): String {
    val totalSeconds = frames.toDouble() / sampleRate
    val minutes = (totalSeconds / 60).toInt()
    val seconds = totalSeconds - minutes * 60
    return String.format(Locale.US, "%d:%04.1f", minutes, seconds)
}
