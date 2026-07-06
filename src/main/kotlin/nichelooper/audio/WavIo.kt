package nichelooper.audio

import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * Minimal RIFF/WAVE reader + writer, enough for the loop library: the writer
 * emits mono IEEE-float32; the reader accepts PCM 16/24/32 and float32 with
 * any channel count (downmixed to mono by averaging).
 */
object WavIo {

    data class Decoded(val samples: FloatArray, val sampleRate: Int)

    /** Writes [samples] as mono float32 WAV at [sampleRate]. */
    fun write(file: File, samples: FloatArray, sampleRate: Int) {
        // Header: RIFF(12) + fmt(24) + fact(12) + data header(8) = 56 bytes.
        val dataBytes = samples.size * 4
        val buffer = ByteBuffer.allocate(56 + dataBytes).order(ByteOrder.LITTLE_ENDIAN)
        buffer.put("RIFF".toByteArray())
        buffer.putInt(48 + dataBytes)          // file size minus RIFF tag + size field
        buffer.put("WAVE".toByteArray())
        buffer.put("fmt ".toByteArray())
        buffer.putInt(16)
        buffer.putShort(3)                     // WAVE_FORMAT_IEEE_FLOAT
        buffer.putShort(1)                     // mono
        buffer.putInt(sampleRate)
        buffer.putInt(sampleRate * 4)          // byte rate
        buffer.putShort(4)                     // block align
        buffer.putShort(32)                    // bits per sample
        buffer.put("fact".toByteArray())       // required for non-PCM formats
        buffer.putInt(4)
        buffer.putInt(samples.size)
        buffer.put("data".toByteArray())
        buffer.putInt(dataBytes)
        for (s in samples) {
            buffer.putFloat(s)
        }
        file.writeBytes(buffer.array())
    }

    /** Duration in ms from the header only (no sample data is read). */
    fun durationMs(file: File): Long = runCatching {
        RandomAccessFile(file, "r").use { raf ->
            val header = readChunks(raf) ?: return@use 0L
            val (fmt, dataSize) = header
            val frames = dataSize / (fmt.channels * fmt.bytesPerSample)
            frames * 1000L / fmt.sampleRate
        }
    }.getOrDefault(0L)

    /** Reads the file and downmixes to mono (averaging channels). */
    fun read(file: File): Decoded {
        RandomAccessFile(file, "r").use { raf ->
            val header = readChunks(raf) ?: error("${file.name} ist kein lesbares WAV")
            val (fmt, dataSize) = header
            val bytes = ByteArray(dataSize)
            raf.seek(fmt.dataOffset)
            raf.readFully(bytes)
            val buffer = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)

            val bytesPerFrame = fmt.channels * fmt.bytesPerSample
            val frames = dataSize / bytesPerFrame
            val mono = FloatArray(frames)
            for (i in 0 until frames) {
                var sum = 0f
                for (c in 0 until fmt.channels) {
                    sum += when {
                        fmt.float32 -> buffer.getFloat(i * bytesPerFrame + c * 4)
                        fmt.bytesPerSample == 2 ->
                            buffer.getShort(i * bytesPerFrame + c * 2) / 32768f
                        fmt.bytesPerSample == 3 -> {
                            val base = i * bytesPerFrame + c * 3
                            val v = (buffer.get(base).toInt() and 0xFF) or
                                ((buffer.get(base + 1).toInt() and 0xFF) shl 8) or
                                (buffer.get(base + 2).toInt() shl 16)
                            v / 8388608f
                        }
                        else -> buffer.getInt(i * bytesPerFrame + c * 4) / 2147483648f
                    }
                }
                mono[i] = sum / fmt.channels
            }
            return Decoded(mono, fmt.sampleRate)
        }
    }

    private class Format(
        val channels: Int,
        val sampleRate: Int,
        val bytesPerSample: Int,
        val float32: Boolean,
        var dataOffset: Long = 0,
    )

    /** Walks the RIFF chunks; returns the parsed fmt + the data chunk size. */
    private fun readChunks(raf: RandomAccessFile): Pair<Format, Int>? {
        fun readLeInt(): Int {
            val b = ByteArray(4)
            raf.readFully(b)
            return ByteBuffer.wrap(b).order(ByteOrder.LITTLE_ENDIAN).int
        }

        fun readTag(): String {
            val b = ByteArray(4)
            raf.readFully(b)
            return String(b, Charsets.US_ASCII)
        }

        if (readTag() != "RIFF") return null
        readLeInt()
        if (readTag() != "WAVE") return null

        var format: Format? = null
        while (raf.filePointer + 8 <= raf.length()) {
            val tag = readTag()
            val size = readLeInt()
            val next = raf.filePointer + size + (size and 1)  // chunks are word-aligned
            when (tag) {
                "fmt " -> {
                    val body = ByteArray(size)
                    raf.readFully(body)
                    val fmt = ByteBuffer.wrap(body).order(ByteOrder.LITTLE_ENDIAN)
                    var audioFormat = fmt.getShort(0).toInt() and 0xFFFF
                    val channels = fmt.getShort(2).toInt()
                    val sampleRate = fmt.getInt(4)
                    val bits = fmt.getShort(14).toInt()
                    if (audioFormat == 0xFFFE && size >= 40) {
                        // WAVE_FORMAT_EXTENSIBLE: the real format is the first
                        // word of the SubFormat GUID at offset 24.
                        audioFormat = fmt.getShort(24).toInt() and 0xFFFF
                    }
                    val float32 = audioFormat == 3 && bits == 32
                    val pcm = audioFormat == 1 && bits in setOf(16, 24, 32)
                    if (!float32 && !pcm) return null
                    if (channels < 1 || sampleRate <= 0) return null
                    format = Format(channels, sampleRate, bits / 8, float32)
                }
                "data" -> {
                    val fmt = format ?: return null
                    fmt.dataOffset = raf.filePointer
                    return fmt to size
                }
            }
            if (next > raf.length()) return null
            raf.seek(next)
        }
        return null
    }
}
