package nichelooper.audio

import net.sourceforge.jaad.aac.AACException
import net.sourceforge.jaad.aac.Receiver
import net.sourceforge.jaad.mp4.MP4Container
import net.sourceforge.jaad.mp4.MP4InputStream
import net.sourceforge.jaad.mp4.api.AudioTrack
import java.io.ByteArrayInputStream
import java.io.EOFException
import java.io.File
import java.io.IOException
import net.sourceforge.jaad.aac.Decoder as AacDecoder

/**
 * .m4a (AAC) in plain Java, for machines without macOS's afconvert. Decodes to mono float like [WavIo.read]:
 * channels are averaged.
 */
object M4aDecoder {

    fun read(file: File): WavIo.Decoded {
        try {
            val movie = MP4Container(MP4InputStream.open(StrictInput(file.readBytes()))).movie
            val track = movie.getTracks(AudioTrack.AudioCodec.AAC).firstOrNull() as? AudioTrack
                ?: error("${file.name} enthält keine AAC-Tonspur")
            val decoder = AacDecoder.create(track.decoderSpecificInfo.data)
            val receiver = MonoReceiver(track.channelCount)
            try {
                while (track.hasMoreFrames()) receiver.decode(decoder, (track.readNextFrame() ?: break).data)
            } catch (e: IOException) {
                // A cut-off file: keep what was decoded up to there.
            }
            return receiver.build() ?: error("Keine Audiodaten in ${file.name}")
        } catch (e: OutOfMemoryError) {
            // A damaged file can announce a block of gigabytes; that is a file we cannot read, not a reason to crash.
            error("${file.name} ist beschädigt")
        }
    }

    /**
     * jaad's MP4 reader skips over a block by calling skip() until enough bytes are gone, which never ends
     * once a cut-off file has no more of them; here skipping past the end is an error.
     */
    private class StrictInput(bytes: ByteArray) : ByteArrayInputStream(bytes) {
        override fun skip(n: Long): Long {
            if (n > available()) throw EOFException("skip past the end of the file")
            return super.skip(n)
        }
    }

    /** jaad hands over each frame as float samples on the 16-bit scale. [channels] is what the file says it has. */
    private class MonoReceiver(private val channels: Int) : Receiver {
        private var samples = FloatArray(0)
        private var length = 0
        private var sampleRate = 0

        fun decode(decoder: AacDecoder, frame: ByteArray) {
            try {
                decoder.decodeFrame(frame, this)
            } catch (e: AACException) {
                // A damaged frame is skipped.
            }
        }

        override fun accept(data: List<FloatArray>, length: Int, sampleRate: Int) {
            if (length <= 0 || data.isEmpty()) return
            if (this.sampleRate == 0) this.sampleRate = sampleRate
            // For a mono file jaad still hands over two arrays, the second a copy of the first.
            val used = minOf(channels.coerceAtLeast(1), data.size)
            reserve(length)
            for (f in 0 until length) {
                var sum = 0f
                for (c in 0 until used) {
                    val channel = data[c]
                    sum += channel[channel.size * f / length]
                }
                samples[this.length + f] = sum / used / 32768f
            }
            this.length += length
        }

        private fun reserve(frames: Int) {
            if (this.length + frames <= samples.size) return
            samples = samples.copyOf(maxOf(samples.size * 2, this.length + frames, 44100))
        }

        fun build(): WavIo.Decoded? =
            if (length == 0 || sampleRate <= 0) null else WavIo.Decoded(samples.copyOf(length), sampleRate)
    }
}
