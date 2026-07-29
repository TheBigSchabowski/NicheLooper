package nichelooper

import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.type
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Window
import androidx.compose.ui.window.application
import androidx.compose.ui.window.rememberWindowState
import nichelooper.ui.TransportScreen
import nichelooper.ui.TransportViewModel

fun main() = application {
    val viewModel = remember { TransportViewModel() }
    Window(
        onCloseRequest = {
            viewModel.shutdown()
            exitApplication()
        },
        title = "NicheLooper",
        state = rememberWindowState(width = 540.dp, height = 900.dp),
        // onKeyEvent, NOT onPreviewKeyEvent: preview fires ahead of the
        // focused component, so typing a preset name would switch chains and
        // mute the drums letter by letter. Here the text field consumes its
        // keystrokes first and the shortcuts only fire when nothing has focus.
        onKeyEvent = { event ->
            // A/S/D switch the live plugin chain, M drops the drums out.
            if (event.type == KeyEventType.KeyDown) {
                when (event.key) {
                    Key.A -> { viewModel.setActiveChain(0); true }
                    Key.S -> { viewModel.setActiveChain(1); true }
                    Key.D -> { viewModel.setActiveChain(2); true }
                    Key.M -> { viewModel.toggleDrumsMute(); true }
                    else -> false
                }
            } else {
                false
            }
        },
    ) {
        MaterialTheme(colorScheme = darkColorScheme()) {
            Surface(modifier = Modifier.fillMaxSize()) {
                TransportScreen(viewModel)
            }
        }
    }
}
