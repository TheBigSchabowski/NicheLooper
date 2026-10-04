package nichelooper

import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.painter.BitmapPainter
import androidx.compose.ui.input.key.Key
import androidx.compose.ui.input.key.KeyEventType
import androidx.compose.ui.input.key.key
import androidx.compose.ui.input.key.type
import androidx.compose.ui.res.loadImageBitmap
import androidx.compose.ui.res.useResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Window
import androidx.compose.ui.window.application
import androidx.compose.ui.window.rememberWindowState
import nichelooper.platform.Platform
import nichelooper.ui.TransportScreen
import nichelooper.ui.TransportViewModel

// macOS takes the icon from the app bundle; the other desktops need it handed to the window.
private fun windowIcon() =
    if (Platform.isMac) null else useResource("icon.png") { BitmapPainter(loadImageBitmap(it)) }

// A/S/D switch the live plugin chain, M drops the drums out.
internal val ShortcutKeys = setOf(Key.A, Key.S, Key.D, Key.M)

fun main() = application {
    val viewModel = remember { TransportViewModel() }
    val icon = remember(::windowIcon)
    Window(
        onCloseRequest = {
            viewModel.shutdown()
            exitApplication()
        },
        title = "NicheLooper",
        icon = icon,
        state = rememberWindowState(width = 540.dp, height = 900.dp),
        // onKeyEvent, NOT onPreviewKeyEvent: preview fires ahead of the
        // focused component, so typing a preset name would switch chains and
        // mute the drums letter by letter. A text field passes the KeyDown of
        // a plain letter on, though, so the preset name field swallows
        // ShortcutKeys itself.
        onKeyEvent = { event ->
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
