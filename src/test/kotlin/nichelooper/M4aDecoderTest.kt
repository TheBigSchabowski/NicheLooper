package nichelooper

import nichelooper.audio.M4aDecoder
import java.io.File
import kotlin.concurrent.thread
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.sin
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertFailsWith
import kotlin.test.assertTrue

/**
 * The files in resources/audio are 0.4 s of a 440 Hz tone, the stereo one with 660 Hz on the right
 * channel; ffmpeg made them. The tests listen for the tones.
 */
class M4aDecoderTest {
    private fun resource(name: String): File {
        val file = File.createTempFile("nichelooper-test", ".m4a")
        file.deleteOnExit()
        checkNotNull(javaClass.getResourceAsStream("/audio/$name")) { "$name fehlt" }
            .use { input -> file.outputStream().use { input.copyTo(it) } }
        return file
    }

    /** Signal strength of [hz] in the frames [from] until [to] (Goertzel); a sine of amplitude a gives a² / 4. */
    private fun strength(samples: FloatArray, from: Int, to: Int, rate: Int, hz: Double): Double {
        var re = 0.0
        var im = 0.0
        for (i in from until to) {
            val phase = 2 * PI * hz * i / rate
            re += samples[i] * cos(phase)
            im += samples[i] * sin(phase)
        }
        val n = (to - from).toDouble()
        return (re * re + im * im) / (n * n)
    }

    @Test
    fun aMonoFileGivesItsToneAtItsOwnRate() {
        val decoded = M4aDecoder.read(resource("mono-22k.m4a"))
        assertEquals(22050, decoded.sampleRate)
        val seconds = decoded.samples.size.toDouble() / decoded.sampleRate
        assertTrue(abs(seconds - 0.4) < 0.15, "Dauer $seconds")
        val from = decoded.samples.size * 3 / 10
        val to = decoded.samples.size * 7 / 10
        val tone = strength(decoded.samples, from, to, decoded.sampleRate, 440.0)
        val other = strength(decoded.samples, from, to, decoded.sampleRate, 1000.0)
        assertTrue(tone > 0.03 && tone > 50 * other, "440 Hz $tone, 1000 Hz $other")
    }

    @Test
    fun aStereoFileIsMixedDownToBothTones() {
        val decoded = M4aDecoder.read(resource("stereo-44k.m4a"))
        assertEquals(44100, decoded.sampleRate)
        val from = decoded.samples.size * 3 / 10
        val to = decoded.samples.size * 7 / 10
        val left = strength(decoded.samples, from, to, decoded.sampleRate, 440.0)
        val right = strength(decoded.samples, from, to, decoded.sampleRate, 660.0)
        val other = strength(decoded.samples, from, to, decoded.sampleRate, 1000.0)
        assertTrue(left > 0.005 && right > 0.005, "440 Hz $left, 660 Hz $right")
        assertTrue(left > 50 * other && right > 50 * other, "1000 Hz $other")
    }

    private fun tempFile(bytes: ByteArray): File {
        val file = File.createTempFile("nichelooper-test", ".m4a")
        file.deleteOnExit()
        file.writeBytes(bytes)
        return file
    }

    // Whatever a damaged file leads to - audio up to the damage or an error - it must not hang or crash.
    @Test
    fun aCutOffFileEndsWithAResultOrAnErrorAndNeverHangs() {
        val bytes = resource("stereo-44k.m4a").readBytes()
        for (share in listOf(0.2, 0.5, 0.8, 0.95)) {
            val cut = tempFile(bytes.copyOf((bytes.size * share).toInt()))
            var outcome: Result<*>? = null
            val worker = thread(isDaemon = true) { outcome = runCatching { M4aDecoder.read(cut) } }
            worker.join(20_000)
            assertTrue(!worker.isAlive, "${(share * 100).toInt()} % gekürzt: hängt")
            assertTrue(outcome != null, "${(share * 100).toInt()} % gekürzt: kein Ergebnis")
        }
    }

    @Test
    fun somethingThatIsNoM4aFails() {
        val junk = tempFile(ByteArray(2000) { (it * 31).toByte() })
        assertFailsWith<Exception> { M4aDecoder.read(junk) }
    }
}
