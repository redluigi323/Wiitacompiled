import shutil
import subprocess
from pathlib import Path

from elftools.elf.elffile import ELFFile

PF_X = 1
PF_RX = 5


class ElfParser:
    def __init__(self, filename):
        self.filename = filename
        path = Path(filename)
        # vita-elf-create embeds this name in the loaded module (not "eboot.bin").
        self.module_names = {path.name, path.stem, "eboot.bin"}
        with open(filename, "rb") as f:
            self.elf = ELFFile(f)
            self.rx_vaddr = -1
            self.entry = self.elf.header["e_entry"]
            self.thumb_image = bool(self.entry & 1)
            self.rx_sizes = set()
            self._collect_rx(self.elf)
        # The .velf is the image the Vita actually maps. vita-elf-create appends
        # the module info and import stubs, so its RX size matches the dump
        # even though the unstripped ELF is shorter. The link address is unchanged.
        velf = path.with_name(path.name + ".velf")
        if velf.is_file():
            with open(velf, "rb") as f:
                try:
                    self._collect_rx(ELFFile(f))
                except Exception:
                    pass
        self.a2l = None

    def _collect_rx(self, elf):
        for seg in elf.iter_segments():
            if seg["p_type"] != "PT_LOAD" or seg["p_flags"] != PF_RX:
                continue
            if self.rx_vaddr < 0:
                self.rx_vaddr = seg["p_vaddr"]
            self.rx_sizes.add(seg["p_memsz"])

    def matches_module(self, module):
        if module.name in self.module_names or module.name.endswith(".elf"):
            return True
        # Same image, relocated. The dump's RX size is the velf's, not the
        # link address.
        for segment in module.segments:
            if (segment.attr & 0xF) == PF_RX and segment.size in self.rx_sizes:
                return True
        return False

    def is_code_segment(self, segment):
        return (segment.attr & PF_X) != 0

    def _tool(self, name):
        path = shutil.which(name)
        if path is None:
            raise SystemExit(
                f"mkwv: {name} not found. Install the Vita SDK and put it on PATH "
                f"(VITASDK/bin, usually /usr/local/vitasdk/bin)."
            )
        return path

    def disas_around_addr(self, addr):
        """Addr is an offset in the executable segment. The low bit selects Thumb."""
        thumb = (addr & 1) != 0 or self.thumb_image
        addr = (addr & ~1) + self.rx_vaddr
        start = addr - 0x10
        end = addr + 0x10
        args = [
            self._tool("arm-vita-eabi-objdump"),
            "-d",
            "-S",
            f"--start-address=0x{start:x}",
            f"--stop-address=0x{end:x}",
        ]
        if thumb:
            args.append("-Mforce-thumb")
        args.append(self.filename)
        output = subprocess.check_output(args, text=True, errors="replace")
        lines = []
        keep = False
        for line in output.splitlines():
            if "Disassembly of section" in line:
                keep = True
                continue
            if keep:
                lines.append(line)
        shown = []
        mark = f"{addr:x}:"
        for line in lines:
            if mark in line:
                body = line[line.find("\t"):] if "\t" in line else line
                shown.append("\033[91m" + f"!!! \t{body} !!!" + "\033[0m")
            else:
                shown.append("\033[90m" + line + "\033[0m")
        print("\n".join(shown))

    def open_addr2line(self):
        if self.a2l is not None:
            return
        self.a2l = subprocess.Popen(
            [self._tool("arm-vita-eabi-addr2line"), "-e", self.filename, "-f", "-p", "-C"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            text=True,
        )

    def close_addr2line(self):
        if self.a2l is not None:
            self.a2l.kill()
            self.a2l = None

    def addr2line(self, addr):
        """Addr is an offset in the executable segment."""
        self.open_addr2line()
        addr = (addr & ~1) + self.rx_vaddr
        self.a2l.stdin.write(f"{hex(addr)}\n")
        self.a2l.stdin.flush()
        return self.a2l.stdout.readline().strip()
