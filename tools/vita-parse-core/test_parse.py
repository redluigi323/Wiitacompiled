"""Build a tiny .psp2dmp and resolve a crash against the unstripped Vita ELF."""
import gzip
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ELF = ROOT / "build-vita" / "vita" / "wiicompiled-vita"
MAIN = Path(__file__).resolve().parent / "main.py"
# vertex_worker_main(), with the Thumb bit set the way a Vita PC stores it.
PC = 0x83243C11
LR = 0x810041D1
SP = 0x81140000


def align4(n):
    return (n + 3) & ~3


def note(name, desc):
    name_b = name.encode("ascii") + b"\0"
    hdr = struct.pack("<III", len(name_b), len(desc), 0)
    name_b += b"\0" * (align4(len(name_b)) - len(name_b))
    desc += b"\0" * (align4(len(desc)) - len(desc))
    return hdr + name_b + desc


def module_info(name=b"eboot.bin", start=0x81000000):
    head = bytearray(0x50)
    struct.pack_into("<I", head, 4, 0x100)
    head[0x24:0x24 + len(name)] = name
    struct.pack_into("<I", head, 0x4C, 1)
    seg = bytearray(0x14)
    struct.pack_into("<I", seg, 4, 5)
    struct.pack_into("<I", seg, 8, start)
    struct.pack_into("<I", seg, 12, 0x02400000)
    struct.pack_into("<I", seg, 16, 0x1000)
    body = struct.pack("<II", 0, 1) + bytes(head) + bytes(seg) + bytes(0x10)
    return body


def thread_info(pc=PC):
    rec = bytearray(0xA0)
    struct.pack_into("<I", rec, 0, len(rec))
    struct.pack_into("<I", rec, 4, 0x40010001)
    rec[8:13] = b"CRASH"
    struct.pack_into("<H", rec, 0x30, 1)
    struct.pack_into("<I", rec, 0x74, 0x30004)
    struct.pack_into("<I", rec, 0x9C, pc)
    return struct.pack("<II", 0, 1) + bytes(rec)


def reg_info():
    rec = bytearray(4 + 4 + 16 * 4)
    struct.pack_into("<I", rec, 0, len(rec))
    struct.pack_into("<I", rec, 4, 0x40010001)
    gpr = [0] * 16
    gpr[13] = SP
    gpr[14] = LR
    gpr[15] = PC
    for i, value in enumerate(gpr):
        struct.pack_into("<I", rec, 8 + 4 * i, value)
    return struct.pack("<II", 0, 1) + bytes(rec)


def elf_core(notes, load_vaddr, load):
    phoff = 52
    note_off = phoff + 32 * 2
    load_off = note_off + len(notes)
    ehdr = struct.pack(
        "<16sHHIIIIIHHHHHH",
        b"\x7fELF\x01\x01\x01" + b"\0" * 9,
        4,  # ET_CORE
        40,  # EM_ARM
        1,
        0,
        phoff,
        0,
        0,
        52,
        32,
        2,
        0,
        0,
        0,
    )
    phdrs = struct.pack(
        "<8I",
        4, note_off, 0, 0, len(notes), len(notes), 4, 4,
    ) + struct.pack(
        "<8I",
        1, load_off, load_vaddr, load_vaddr, len(load), len(load), 6, 4,
    )
    return ehdr + phdrs + notes + load


def check(blob, label, needles):
    with tempfile.TemporaryDirectory() as tmp:
        raw = Path(tmp) / "crash.psp2dmp"
        raw.write_bytes(blob)
        out = subprocess.check_output(
            [sys.executable, str(MAIN), "-s", "4", str(raw), str(ELF)],
            text=True,
            errors="replace",
        )
        print(f"--- {label} ({len(out)} bytes) ---")
        for needle in needles:
            if needle not in out:
                print(out)
                sys.exit(f"{label}: missing {needle!r}")
    print(f"{label} ok")


def main():
    if not ELF.is_file():
        sys.exit(f"missing {ELF}")
    stack = bytearray((16 + 24) * 4)
    struct.pack_into("<I", stack, 16 * 4, PC)
    notes = note("MODULE_INFO", module_info()) + note("THREAD_INFO", thread_info()) + note("THREAD_REG_INFO", reg_info())
    check(elf_core(notes, SP - 16 * 4, bytes(stack)), "eboot-at-link-address", (
        "CRASH",
        "Data abort exception",
        "eboot.bin@1",
        "vertex_worker_main",
        "SP =>",
        "DISASSEMBLY AROUND PC",
    ))
    # The Vita relocates the RX segment and names it after the ELF, not eboot.bin.
    # vertex_worker_main is 0x2243c11 bytes into the image; slide the base by 0x41000.
    relocated_pc = 0x81041000 + (PC - 0x81000000)
    notes = (
        note("MODULE_INFO", module_info(b"wiicompiled-vita", 0x81041000))
        + note("THREAD_INFO", thread_info(relocated_pc))
        + note("THREAD_REG_INFO", reg_info())
    )
    stack = bytearray((16 + 24) * 4)
    struct.pack_into("<I", stack, 16 * 4, relocated_pc)
    check(elf_core(notes, SP - 16 * 4, bytes(stack)), "relocated-module-name", (
        "wiicompiled-vita@1",
        "vertex_worker_main",
        "DISASSEMBLY AROUND PC",
    ))
    print("ok")


if __name__ == "__main__":
    main()
