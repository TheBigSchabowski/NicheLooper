package nichelooper.audio

import java.io.File
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale

/**
 * Stores a mono float loop as lossless float32 WAV in ~/Music/NicheLooper.
 * Runs entirely off the audio path — the engine keeps playing while this
 * writes. (The Android app encodes AAC via MediaCodec; on the Mac WAV is
 * simpler, lossless, and every DAW reads it.)
 */
object LoopSaver {

    val directory = File(System.getProperty("user.home"), "Music/NicheLooper")

    /** Returns the user-visible location of the saved file. */
    fun save(samples: FloatArray, sampleRate: Int): Result<String> = runCatching {
        check(directory.isDirectory || directory.mkdirs()) {
            "Kann ${directory.absolutePath} nicht anlegen"
        }
        val fileName = "Loop_" +
            SimpleDateFormat("yyyyMMdd_HHmmss", Locale.US).format(Date()) + ".wav"
        val file = File(directory, fileName)
        WavIo.write(file, samples, sampleRate)
        "Music/NicheLooper/$fileName"
    }
}
