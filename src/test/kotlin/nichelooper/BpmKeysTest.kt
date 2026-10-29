package nichelooper

import androidx.compose.ui.input.key.Key
import nichelooper.ui.BpmKeys
import nichelooper.ui.TransportViewModel
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull

class BpmKeysTest {

    @Test
    fun `with stores and replaces a tempo per key`() {
        val keys = BpmKeys().with(Key.N, 99).with(Key.M, 120).with(Key.N, 101)
        assertEquals(101, keys.bpmFor(Key.N))
        assertEquals(120, keys.bpmFor(Key.M))
        assertNull(keys.bpmFor(Key.B))
        assertEquals(listOf(Key.N to 101, Key.M to 120), keys.entries)
    }

    @Test
    fun `tempo is clamped to the engine range`() {
        val keys = BpmKeys().with(Key.N, 5).with(Key.M, 999)
        assertEquals(TransportViewModel.MIN_BPM, keys.bpmFor(Key.N))
        assertEquals(TransportViewModel.MAX_BPM, keys.bpmFor(Key.M))
    }

    @Test
    fun `serialize and parse round-trip`() {
        val keys = BpmKeys().with(Key.N, 99).with(Key.F1, 140).without(Key.F1).with(Key.Seven, 70)
        val parsed = BpmKeys.parse(keys.serialize())
        assertEquals(keys, parsed)
    }

    @Test
    fun `parse skips broken lines`() {
        val parsed = BpmKeys.parse("# comment\nN=99\nnokey=80\nM=fast\n=100\nV=88\n")
        assertEquals(mapOf(Key.N to 99, Key.V to 88), parsed.tempos)
    }
}
