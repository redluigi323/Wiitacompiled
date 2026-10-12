import string
import struct


def _as_bytes(buf):
    if isinstance(buf, str):
        return buf.encode("latin1")
    return bytes(buf)


def u16(buf, off):
    return struct.unpack_from("<H", _as_bytes(buf), off)[0]


def u32(buf, off):
    return struct.unpack_from("<I", _as_bytes(buf), off)[0]


def c_str(buf, off):
    raw = _as_bytes(buf)
    end = raw.find(b"\0", off)
    if end < 0:
        end = len(raw)
    return raw[off:end].decode("latin1", errors="replace")


def hexdump(src, length=16, sep="."):
    raw = _as_bytes(src)
    lines = []
    for c in range(0, len(raw), length):
        chars = raw[c:c + length]
        hex_part = " ".join(f"{b:02x}" for b in chars)
        if len(hex_part) > 24:
            hex_part = f"{hex_part[:24]} {hex_part[24:]}"
        printable = "".join(chr(b) if 32 <= b < 127 else sep for b in chars)
        lines.append(f"{c:08x}:  {hex_part:<{length * 3}}  |{printable}|\n")
    print("".join(lines))
