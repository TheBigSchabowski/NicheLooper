package nichelooper.audio

import java.io.File

data class SavedLoop(
    val file: File,
    val name: String,
    val durationMs: Long,
)

/**
 * Lists loops in ~/Music/NicheLooper and decodes them back to mono float PCM
 * at the engine sample rate. Decoding runs off the audio path.
 *
 * WAV is read directly; .m4a (e.g. loops saved by the Android app and copied
 * over) is converted through macOS's bundled `afconvert` first.
 */
object LoopLibrary {

    fun list(): List<SavedLoop> =
        LoopSaver.directory
            .listFiles { f -> f.isFile && f.extension.lowercase() in SUPPORTED }
            ?.sortedByDescending { it.lastModified() }
            ?.map { file ->
                SavedLoop(
                    file = file,
                    name = file.name,
                    durationMs = if (file.extension.equals("wav", true)) {
                        WavIo.durationMs(file)
                    } else {
                        0L
                    },
                )
            }
            ?: emptyList()

    /** Decodes to mono float at [targetSampleRate], capped to [maxFrames]. */
    fun decode(loop: SavedLoop, targetSampleRate: Int, maxFrames: Int): FloatArray {
        val wavFile =
            if (loop.file.extension.equals("m4a", true)) convertToWav(loop.file) else loop.file
        try {
            val decoded = WavIo.read(wavFile)
            val samples =
                if (decoded.sampleRate == targetSampleRate) decoded.samples
                else resampleLinear(decoded.samples, decoded.sampleRate, targetSampleRate)
            check(samples.isNotEmpty()) { "Keine Audiodaten in ${loop.name}" }
            return if (samples.size > maxFrames) samples.copyOf(maxFrames) else samples
        } finally {
            if (wavFile != loop.file) wavFile.delete()
        }
    }

    private fun convertToWav(source: File): File {
        val temp = File.createTempFile("nichelooper", ".wav")
        val process = ProcessBuilder(
            "/usr/bin/afconvert", "-f", "WAVE", "-d", "LEF32",
            source.absolutePath, temp.absolutePath,
        ).redirectErrorStream(true).start()
        val output = process.inputStream.bufferedReader().readText()
        check(process.waitFor() == 0) { "afconvert fehlgeschlagen: ${output.trim()}" }
        return temp
    }

    private fun resampleLinear(input: FloatArray, fromRate: Int, toRate: Int): FloatArray {
        val outLength = (input.size.toLong() * toRate / fromRate).toInt()
        if (outLength <= 0) return FloatArray(0)
        val output = FloatArray(outLength)
        val step = fromRate.toDouble() / toRate
        for (i in output.indices) {
            val pos = i * step
            val idx = pos.toInt()
            val frac = (pos - idx).toFloat()
            val a = input[idx.coerceAtMost(input.lastIndex)]
            val b = input[(idx + 1).coerceAtMost(input.lastIndex)]
            output[i] = a + (b - a) * frac
        }
        return output
    }

    private val SUPPORTED = setOf("wav", "m4a")
}
