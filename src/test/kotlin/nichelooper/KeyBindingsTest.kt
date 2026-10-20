package nichelooper

import androidx.compose.ui.input.key.Key
import nichelooper.ui.KeyAction
import nichelooper.ui.KeyBindings
import kotlin.test.Test
import kotlin.test.assertEquals
import kotlin.test.assertNull
import kotlin.test.assertTrue

class KeyBindingsTest {

    @Test
    fun `default bindings assign Spacebar to REC_STOP and ASD to chains`() {
        val defaults = KeyBindings()
        assertTrue(defaults.isDefault)
        assertEquals(Key.Spacebar, defaults.keyFor(KeyAction.REC_STOP))
        assertEquals("Leertaste", defaults.labelFor(KeyAction.REC_STOP))
        assertEquals(KeyAction.REC_STOP, defaults.actionFor(Key.Spacebar))
        assertEquals(Key.A, defaults.keyFor(KeyAction.CHAIN_1))
        assertEquals(Key.S, defaults.keyFor(KeyAction.CHAIN_2))
        assertEquals(Key.D, defaults.keyFor(KeyAction.CHAIN_3))
        assertEquals(Key.M, defaults.keyFor(KeyAction.DRUMS_MUTE))
    }

    @Test
    fun `withKey swaps keys when assigning an already bound key`() {
        val defaults = KeyBindings()
        val swapped = defaults.withKey(KeyAction.REC_STOP, Key.A)
        assertEquals(Key.A, swapped.keyFor(KeyAction.REC_STOP))
        assertEquals(Key.Spacebar, swapped.keyFor(KeyAction.CHAIN_1))
        assertEquals(KeyAction.REC_STOP, swapped.actionFor(Key.A))
        assertEquals(KeyAction.CHAIN_1, swapped.actionFor(Key.Spacebar))
    }

    @Test
    fun `withKey null unbinds action`() {
        val unbound = KeyBindings().withKey(KeyAction.REC_STOP, null)
        assertNull(unbound.keyFor(KeyAction.REC_STOP))
        assertEquals("—", unbound.labelFor(KeyAction.REC_STOP))
        assertNull(unbound.actionFor(Key.Spacebar))
    }

    @Test
    fun `serialize and parse round-trip custom and unbound keys`() {
        val custom = KeyBindings()
            .withKey(KeyAction.REC_STOP, Key.F1)
            .withKey(KeyAction.OVERDUB, Key.Spacebar)
            .withKey(KeyAction.CLEAR, null)

        val serialized = custom.serialize()
        val parsed = KeyBindings.parse(serialized)

        assertEquals(Key.F1, parsed.keyFor(KeyAction.REC_STOP))
        assertEquals(Key.Spacebar, parsed.keyFor(KeyAction.OVERDUB))
        assertNull(parsed.keyFor(KeyAction.CLEAR))
        assertEquals(Key.A, parsed.keyFor(KeyAction.CHAIN_1))
    }

    @Test
    fun `parse avoids duplicate assignment when partial config reuses a default key`() {
        val partial = "chain_1=Space\n"
        val parsed = KeyBindings.parse(partial)
        assertEquals(Key.Spacebar, parsed.keyFor(KeyAction.CHAIN_1))
        assertNull(parsed.keyFor(KeyAction.REC_STOP))
    }
}
