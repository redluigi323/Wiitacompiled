import gzip

from elftools.elf.elffile import ELFFile

from indent import iprint
from util import u16, u32, c_str


class VitaThread:
    def __init__(self, data):
        self.uid = u32(data, 4)
        self.name = c_str(data, 8)
        self.stop_reason = u32(data, 0x74)
        self.status = u16(data, 0x30)
        self.pc = u32(data, 0x9C)

    def __str__(self):
        return f"{self.name} <0x{self.uid:x}>"


class VitaModuleSegment:
    def __init__(self, data, num):
        self.num = num
        self.attr = u32(data, 4)
        self.start = u32(data, 8)
        self.size = u32(data, 12)
        self.align = u32(data, 16)


class VitaModule:
    def __init__(self, data):
        self.uid = u32(data, 4)
        self.num_segs = u32(data, 0x4C)
        self.name = c_str(data, 0x24)

    def parse_segs(self, data):
        self.segments = []
        for x in range(self.num_segs):
            sz = 0x14
            self.segments.append(VitaModuleSegment(data[sz * x:sz * (x + 1)], x + 1))

    def parse_foot(self, data):
        # arm exception start/end
        pass


class VitaRegs:
    def __init__(self, data):
        self.tid = u32(data, 4)
        self.gpr = [u32(data, 8 + 4 * x) for x in range(16)]


class VitaAddress:
    def __init__(self, symbol, vaddr, module=None, segment=None, offset=None):
        self._symbol = symbol
        self._module = module
        self._segment = segment
        self._offset = offset
        self._vaddr = vaddr

    def is_located(self):
        return self._module and self._segment and self._offset is not None

    def _in_elf(self, elf):
        if elf is None or not self.is_located():
            return False
        # The Vita relocates the RX segment. The offset from that segment's
        # runtime start is still the offset from the ELF's link address.
        return elf.matches_module(self._module) and elf.is_code_segment(self._segment)

    def print_disas_if_available(self, elf):
        addr_to_display = self._vaddr & ~1
        thumb = bool(self._vaddr & 1) or bool(getattr(elf, "thumb_image", False))
        state = "Thumb" if thumb else "ARM"
        if self._in_elf(elf):
            iprint()
            iprint(f"DISASSEMBLY AROUND {self._symbol}: 0x{addr_to_display:x} ({state}):")
            elf.disas_around_addr(self._offset)

    def to_string(self, elf=None):
        if self.is_located():
            output = (
                f"{self._symbol}: 0x{self._vaddr:x} "
                f"({self._module.name}@{self._segment.num} + 0x{self._offset:x}"
            )
            if self._in_elf(elf):
                output += f" => {elf.addr2line(self._offset)}"
            output += ")"
        else:
            output = f"{self._symbol}: 0x{self._vaddr:x}"
        return output

    def __str__(self):
        return self.to_string()


class Segment:
    def __init__(self, vaddr, data):
        self.vaddr = vaddr
        self.data = data
        self.size = len(data)


def _open_core(filename):
    with open(filename, "rb") as raw:
        magic = raw.read(2)
    if magic == b"\x1f\x8b":
        return gzip.open(filename, "rb")
    return open(filename, "rb")


def _note_bytes(desc):
    if isinstance(desc, str):
        return desc.encode("latin1")
    return bytes(desc)


class CoreParser:
    def __init__(self, filename):
        try:
            f = _open_core(filename)
        except OSError as exc:
            raise SystemExit(f"mkwv: cannot open {filename}: {exc}") from exc
        try:
            try:
                self.elf = ELFFile(f)
                self.init_notes()
            except SystemExit:
                raise
            except Exception as exc:
                raise SystemExit(f"mkwv: {filename} is not an ELF coredump ({exc})") from exc
        finally:
            f.close()

        for name in ("MODULE_INFO", "THREAD_INFO", "THREAD_REG_INFO"):
            if name not in self.notes:
                raise SystemExit(f"mkwv: {filename} has no {name} note")

        self.parse_modules()
        self.parse_threads()
        self.parse_thread_regs()

    def init_notes(self):
        self.notes = {}
        self.segments = []

        for seg in self.elf.iter_segments():
            if seg["p_type"] == "PT_NOTE":
                for note in seg.iter_notes():
                    self.notes[note["n_name"]] = _note_bytes(note["n_desc"])
            elif seg["p_type"] == "PT_LOAD":
                self.segments.append(Segment(seg["p_vaddr"], seg.data()))

    def parse_modules(self):
        self.modules = []
        data = self.notes["MODULE_INFO"]
        num = u32(data, 4)
        off = 8
        for _ in range(num):
            sz = 0x50
            module = VitaModule(data[off:off + sz])
            off += sz
            sz = module.num_segs * 0x14
            module.parse_segs(data[off:off + sz])
            off += sz
            sz = 0x10
            module.parse_foot(data[off:off + sz])
            off += sz
            self.modules.append(module)

    def parse_threads(self):
        self.threads = []
        self.tid_to_thread = {}
        data = self.notes["THREAD_INFO"]
        num = u32(data, 4)
        off = 8
        for _ in range(num):
            sz = u32(data, off)
            thread = VitaThread(data[off:off + sz])
            self.threads.append(thread)
            self.tid_to_thread[thread.uid] = thread
            off += sz

    def parse_thread_regs(self):
        data = self.notes["THREAD_REG_INFO"]
        num = u32(data, 4)
        off = 8
        for _ in range(num):
            sz = u32(data, off)
            regs = VitaRegs(data[off:off + sz])
            self.tid_to_thread[regs.tid].regs = regs
            off += sz

    def get_address_notation(self, symbol, vaddr):
        for module in self.modules:
            for segment in module.segments:
                if segment.start <= vaddr < segment.start + segment.size:
                    return VitaAddress(symbol, vaddr, module, segment, vaddr - segment.start)
        return VitaAddress(symbol, vaddr)

    def read_vaddr(self, addr, size):
        for segment in self.segments:
            if segment.vaddr <= addr < segment.vaddr + segment.size:
                addr -= segment.vaddr
                return segment.data[addr:addr + size]
        return None
