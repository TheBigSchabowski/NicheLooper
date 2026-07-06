package nichelooper.audio

import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.atomic.AtomicBoolean

/**
 * Loads the bundled acoustic drum kit (see resources/drums/ATTRIBUTION.txt)
 * into the native engine. The .raw files are mono float32 little-endian at
 * 48 kHz, pre-trimmed and peak-normalized; the engine resamples and scales
 * them when the streams open. If loading fails the engine simply keeps its
 * synthesized fallback drums.
 */
object DrumKitLoader {

    private const val SOURCE_RATE = 48_000

    private val loaded = AtomicBoolean(false)

    /** Idempotent; call before AudioEngine.start(). */
    fun ensureLoaded() {
        if (!loaded.compareAndSet(false, true)) return
        val result = runCatching {
            AudioEngine.setDrumSamples(
                kick = readRawFloats("/drums/kick.raw"),
                snare = readRawFloats("/drums/snare.raw"),
                hat = readRawFloats("/drums/hat.raw"),
                sourceRate = SOURCE_RATE,
            )
        }
        if (result.isFailure) {
            loaded.set(false)  // synth fallback now; retry on the next start
        }
    }

    private fun readRawFloats(path: String): FloatArray {
        val bytes = DrumKitLoader::class.java.getResourceAsStream(path)?.use { it.readBytes() }
            ?: error("Resource $path fehlt")
        val floats = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN).asFloatBuffer()
        return FloatArray(floats.remaining()).also(floats::get)
    }
}
