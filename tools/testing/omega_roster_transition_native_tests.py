"""Replay Omega's captured travel rosters through original native 3CCE50.

World lookup and object creation/removal are observation stubs. The original
generation comparison and roster mirror copy execute; no live process access.
"""
import hashlib
import struct
import unittest
from pathlib import Path
from mercury_streaming_native import Native, DATA, IMAGE_SHA256

ROOT = Path(__file__).resolve().parents[2]


def lighthouse():
    raw = (ROOT / 'Dawn/unit/fixtures/omega_loading_roster.bin').read_bytes()
    assert hashlib.sha256(raw).hexdigest() == 'e5db5928083074c147e4bda5eac2d37ccb49a427f5da8d2f4285d74d1a08d8df'
    count, top, _ = struct.unpack_from('<III', raw)
    at, keys = 12, []
    for _ in range(count):
        key, slots = struct.unpack_from('<IH', raw, at)
        keys.append(key)
        at += 6 + slots * 4
    count, = struct.unpack_from('<I', raw, at)
    at += 4
    blocks = []
    for _ in range(count):
        bubble, size = struct.unpack_from('<II', raw, at)
        at += 8
        block = struct.unpack_from('<' + 'I' * size, raw, at)
        at += 4 * size
        blocks.append((bubble, block))
    assert at == len(raw)
    return keys[:top], blocks


class OmegaTravelNativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = Path(r'C:\Destiny 2 Development\destiny2_unpacked.bin').read_bytes()
        assert hashlib.sha256(cls.image).hexdigest() == IMAGE_SHA256

    def replay(self, old, new, old_state, new_state, bubble):
        n = Native(self.image)
        context, delta = DATA, DATA + 0x10000
        for base, roster, state in ((context + 8, old, old_state), (delta, new, new_state)):
            top, blocks = roster
            n.put(base, len(top))
            for i, key in enumerate(top):
                n.put(base + 4 + i * 4, key)
                n.put(base + 0x428 + i, state, 'B')
            n.put(base + 0x404, (1 << len(top)) - 1)
            n.put(base + 0x424, len(top))
            n.put(base + 0x528, len(blocks))
            for i, (owner, keys) in enumerate(blocks):
                block = base + 0x52c + i * 0x1f8
                n.put(block, owner)
                n.put(block + 4, len(keys))
                n.put(block + 0x188, (1 << len(keys)) - 1)
                n.put(block + 0x194, len(keys))
                for j, key in enumerate(keys):
                    n.put(block + 8 + j * 4, key)
                    n.put(block + 0x198 + j, state, 'B')
        events = []
        n.stubs[0x4294C0] = lambda: DATA + 0x30000
        n.stubs[0x429BA0] = lambda: n.put(n.arg(1), bubble) or n.arg(1)
        n.stubs[0x3CA800] = lambda: events.append(('remove', n.get(n.arg(1))))
        n.stubs[0x3CBBE0] = lambda: events.append(('create', n.get(n.arg(1))))
        n.stubs[0x4EF900] = lambda: None
        n.call(0x3CCE50, context, delta)
        return events

    def test_old_region_change_destroys_all_global_runtime(self):
        old = lighthouse()
        forest = old[0], [(b, k) for b, k in old[1] if b != 15]
        events = self.replay(old, forest, 0x83, 0x84, 11)
        for key in (*old[0], 0x2763EC97):
            self.assertIn(('remove', key), events)
            self.assertIn(('create', key), events)

    def test_retained_ordinals_and_generations_do_not_recreate_runtime(self):
        retained = lighthouse()
        # The production lifetime projection is separately exercised against
        # the complete captured roster in omega_loading_roster_cases.h.
        for bubble in (11, 15, 11, 14):
            with self.subTest(bubble=bubble):
                self.assertEqual(self.replay(retained, retained, 0x83, 0x83, bubble), [])


if __name__ == '__main__':
    unittest.main()
