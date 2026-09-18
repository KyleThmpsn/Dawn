"""Replay the pinned teleport update's departure callback, without a live process.

The animation clock, actor transform service, selector callback and handle table
are fixtures. The real update routine chooses the stage and continuation result.
"""
import argparse
import hashlib
import struct
from pathlib import Path

from mercury_streaming_native import Native, BASE, DATA, IMAGE_SHA256
from unicorn.x86_const import UC_X86_REG_RAX, UC_X86_REG_XMM0


def departure_case(image, callback_continues):
    n = Native(image)
    raw, context, directory, table, rows = [DATA + x for x in (0, 0x1000, 0x2000, 0x3000, 0x4000)]
    n.put(BASE + 0x2439C70, directory, "Q")
    n.put(directory, table, "Q")
    n.put(table + 8, rows, "Q")
    n.put(table + 0x30, 0x100)
    n.put(raw + 0x38, 0xFFFFFFFF)  # No unrelated movement-body services.
    n.put(raw + 0x50, 1)
    n.put(raw + 0x58, 2)
    n.put(raw + 0x98, 3.0, "f")
    n.put(raw + 0xA4, 1, "B")
    callbacks, placements = [], []

    def clock():
        n.uc.reg_write(UC_X86_REG_XMM0, struct.unpack("<I", struct.pack("<f", 3.1))[0])

    def callback():
        callbacks.append(n.arg(1))
        assert n.get(n.arg(0), "Q") == rows + 0x100
        assert n.get(n.arg(0) + 8, "Q") == rows + 0x200
        return int(callback_continues)

    n.stubs.update({
        0xDDA370: lambda: 0,
        0xF48AD0: clock,
        0xF54BF0: lambda: 0,
        0xF50AC0: lambda: None,
        0xF4AAB0: lambda: placements.append(n.arg(0)),
        0x1848E50: callback,
        0x187C480: lambda: None,
    })
    n.call(0x10B0030, 0, context, raw)
    assert callbacks == [2] and placements == [raw]
    assert n.get(raw + 0xA4, "B") == 2
    assert bool(n.uc.reg_read(UC_X86_REG_RAX) & 255) == callback_continues
    print(f"PASS native departure: placement precedes stage-2 callback, continues={callback_continues}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    image = args.image.read_bytes()
    if hashlib.sha256(image).hexdigest() != IMAGE_SHA256:
        raise SystemExit("Unsupported executable image")
    for continues in (False, True):
        departure_case(image, continues)
