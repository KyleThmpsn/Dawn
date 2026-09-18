"""Replay the original game's all-records-seeded gate at Omega's return teleport.

The unmodified 4D6530 loop decides readiness. Pool allocation, iteration setup,
and world lookup use fixtures. This does not claim in-game movie playback.
"""
import hashlib
from pathlib import Path
import unittest
from mercury_streaming_native import Native, DATA, BASE, IMAGE_SHA256
from unicorn.x86_const import UC_X86_REG_RAX


class OmegaEndingSeedNativeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.image = Path(r'C:\Destiny 2 Development\destiny2_unpacked.bin').read_bytes()
        assert hashlib.sha256(cls.image).hexdigest() == IMAGE_SHA256

    def fixture(self):
        n = Native(self.image)
        context, directory, tables = DATA, DATA + 0x1000, DATA + 0x2000
        pool, vtable, descriptor = DATA + 0x4000, DATA + 0x6000, DATA + 0x7000
        bitmap, entries = DATA + 0x7800, DATA + 0x8000
        types = (16, 35, 18, 17, 41) + (13,) * 16 + (6,)
        count, mask = len(types), (1 << len(types)) - 1
        n.put(BASE + 0x2439C70, directory, 'Q')
        n.put(directory, tables, 'Q')
        n.put(tables + 8, entries, 'Q')
        n.put(tables + 0x30, 0x88)
        n.put(tables + 0x34, 0)
        n.put(context + 0x808, pool, 'Q')
        n.put(pool, vtable, 'Q')
        n.put(vtable + 8, descriptor, 'Q')
        n.put(descriptor + 0x10, bitmap, 'Q')
        n.put(pool + 8, entries, 'Q')
        n.put(pool + 0x1C, 0x80)
        n.put(pool + 0x20, 0x88)
        n.put(pool + 0x24, 0xFF)
        n.put(pool + 0x34, 0)
        n.put(bitmap, mask)
        n.stubs[0x1AB6610] = lambda: count
        n.stubs[0x34E830] = lambda: 1

        def iterator():
            out = n.arg(0)
            n.put(out, 0)
            n.put(out + 8, bitmap, 'Q')
            n.put(out + 0x10, count)
            n.put(out + 0x14, mask)

        n.stubs[0x35E460] = iterator
        n.stubs[0x4294C0] = lambda: DATA + 0xB000
        n.stubs[0x429BA0] = lambda: n.put(n.arg(1), 15) or n.arg(1)
        for index, kind in enumerate(types):
            row = entries + index * 0x88
            movie = index == 21
            n.put(row, 0x3A6CE17A if movie else 0x4786C0E0)
            n.put(row + 4, kind, 'H')
            n.put(row + 6, 0 if movie else index, 'H')
            n.put(row + 0x18, 1, 'B')
            n.put(row + 0x68, 15 if movie else 0xFFFFFFFF)
            n.put(row + 0x80, 1)
        return n, context, entries

    def ready(self, n, context):
        n.call(0x4D6530, context)
        return bool(n.uc.reg_read(UC_X86_REG_RAX) & 255)

    def test_movie_and_restriction_bodies_alone_leave_playback_blocked(self):
        n, context, entries = self.fixture()
        # The release's retained-authority branch publishes only restriction
        # slots 1/3 and the movie. Nineteen root records remain uninitialized.
        for slot in (1, 3, 21):
            n.put(entries + slot * 0x88 + 0x18, 0, 'B')
        self.assertFalse(self.ready(n, context))
        for slot in (0, 2, *range(4, 21)):
            self.assertFalse(self.ready(n, context), slot)
            n.put(entries + slot * 0x88 + 0x18, 0, 'B')
        self.assertTrue(self.ready(n, context))
        self.assertTrue(self.ready(n, context))

    def test_initialization_does_not_fabricate_movie_readiness(self):
        n, context, entries = self.fixture()
        for slot in range(21):
            n.put(entries + slot * 0x88 + 0x18, 0, 'B')
        self.assertFalse(self.ready(n, context))
        n.put(entries + 21 * 0x88 + 0x18, 0, 'B')
        self.assertTrue(self.ready(n, context))


if __name__ == '__main__':
    unittest.main()
