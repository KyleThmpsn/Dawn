"""Replay the pinned native damage decision for Hijacked's phase floors.

No live process access. Health accessors are fixtures representing the body
fraction after packet application. The lethal-flag decision, body aggregation,
and death-branch guards execute from the unmodified game image. C++ packet
mutation and mission ownership are covered by hijacked_tests.
"""
import argparse
import hashlib
import struct
from pathlib import Path

from unicorn import UC_HOOK_CODE
from unicorn.x86_const import (
    UC_X86_REG_RDI, UC_X86_REG_R15, UC_X86_REG_RBP, UC_X86_REG_RSP,
    UC_X86_REG_R14, UC_X86_REG_R13, UC_X86_REG_XMM11, UC_X86_REG_XMM0,
    UC_X86_REG_RIP,
)
from mercury_streaming_native import Native, BASE, DATA, STACK, IMAGE_SHA256


def verify(image):
    if hashlib.sha256(image).hexdigest() != IMAGE_SHA256:
        raise ValueError("Unsupported native image SHA256")
    n = Native(image)
    packet, context, health, definition, descriptor, template, frame = (
        DATA + offset for offset in (0, 0x1000, 0x2000, 0x4000, 0x5000, 0x6000, 0x10000)
    )
    sp = STACK + 0x80000
    n.put(context, definition, "Q")
    n.put(context + 8, health, "Q")
    n.put(context + 0x10, health, "Q")
    n.put(definition + 0x368, 1, "Q")
    n.put(frame + 0xA8, context, "Q")
    fraction = [0.0]

    def return_float(value):
        bits = struct.unpack("<I", struct.pack("<f", value))[0]
        n.uc.reg_write(UC_X86_REG_XMM0, bits)

    n.stubs.update({
        0xCD8320: lambda: template,
        0xCD80A0: lambda: 1,
        0xCD7FD0: lambda: descriptor,
        0xB8F7F0: lambda: None,
        0xB8B6E0: lambda: return_float(100.0),
        0xB8B0C0: lambda: return_float(fraction[0]),
    })

    def stop(uc, address, size, data):
        if address - BASE in (0xB80E44, 0xB81618, 0xB81724):
            uc.emu_stop()

    n.uc.hook_add(UC_HOOK_CODE, stop)
    for label, flags, hp, gate, expected in (
        ("old clamp: lethal hit still kills at two-thirds", 1, 2 / 3, 1, True),
        ("old clamp: lethal hit still kills at one-third", 1, 1 / 3, 1, True),
        ("corrected first retreat", 0, 2 / 3, 1, False),
        ("corrected second retreat", 0, 1 / 3, 1, False),
        ("other header flags preserved", 0xA4, 1 / 3, 1, False),
        ("final phase: predicted lethal hit", 1, 0, 1, True),
        ("final phase: native zero-health decision", 0, 0, 1, True),
        ("existing teleport immunity gate", 1, 1 / 3, 0, False),
    ):
        fraction[0] = hp
        n.put(packet + 0x60, flags, "B")
        n.put(health + 0x338, 0, "B")
        n.put(sp + 0x64, gate, "B")
        for register, value in (
            (UC_X86_REG_RDI, packet), (UC_X86_REG_R15, context),
            (UC_X86_REG_RBP, frame), (UC_X86_REG_RSP, sp),
            (UC_X86_REG_R14, 0), (UC_X86_REG_R13, 0), (UC_X86_REG_XMM11, 0),
        ):
            n.uc.reg_write(register, value)
        n.uc.emu_start(BASE + 0xB80D23, BASE + 0xB80E44, count=1000)
        assert n.uc.reg_read(UC_X86_REG_RIP) == BASE + 0xB80E44, label
        n.uc.emu_start(BASE + 0xB815EB, BASE + 0xB90000, count=100)
        end = n.uc.reg_read(UC_X86_REG_RIP) - BASE
        assert end in (0xB81618, 0xB81724), (label, hex(end))
        assert (end == 0xB81618) == expected, label
        print(f"PASS {label}: death_branch={expected}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path, help="Pinned unpacked native image")
    args = parser.parse_args()
    verify(args.image.read_bytes())
