package nichelooper

import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.darkColorScheme
import androidx.compose.runtime.remember
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.painter.BitmapPainter
import androidx.compose.ui.res.loadImageBitmap
import androidx.compose.ui.res.useResource
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.Window
import androidx.compose.ui.window.application
import androidx.compose.ui.window.rememberWindowState
import nichelooper.platform.Platform
import nichelooper.ui.ShortcutDispatcher
import nichelooper.ui.TransportScreen
import nichelooper.ui.TransportViewModel

// macOS takes the icon from the app bundle; the other desktops need it handed to the window.
private fun windowIcon() =
    if (Platform.isMac) null else useResource("icon.png") { BitmapPainter(loadImageBitmap(it)) }

fun main() = application {
    val viewModel = remember { TransportViewModel() }
    val shortcutDispatcher = remember(viewModel) { ShortcutDispatcher(viewModel) }
    val icon = remember(::windowIcon)
    Window(
        onCloseRequest = {
            viewModel.shutdown()
            exitApplication()
        },
        title = "NicheLooper",
        icon = icon,
        state = rememberWindowState(width = 540.dp, height = 900.dp),
        // Handled in onPreviewKeyEvent so Spacebar (Stop / Record) is never
        // eaten by a focused Compose Button/Switch. While a text field inside
        // NicheLooper has focus, ShortcutDispatcher yields immediately; and
        // when a VST3/AU plugin window has OS focus, events go straight to the
        // plugin window and never reach this Compose window.
        onPreviewKeyEvent = shortcutDispatcher::onPreviewKeyEvent,
    ) {
        MaterialTheme(colorScheme = darkColorScheme()) {
            Surface(modifier = Modifier.fillMaxSize()) {
                TransportScreen(viewModel)
            }
        }
    }
}
