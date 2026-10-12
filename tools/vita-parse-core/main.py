import os
from collections import defaultdict
from argparse import ArgumentParser
from pathlib import Path

from util import u32
from indent import indent, iprint
from elf import ElfParser
from core import CoreParser

str_stop_reason = defaultdict(lambda: "Unknown", {
    0: "No reason",
    0x10001: "Suspend process",
    0x10002: "Suspend thread",
    0x10003: "Application is suspended",
    0x10004: "AppMgr detected hung",
    0x10005: "Spontaneous exit",
    0x10006: "Stack overflow",
    0x10007: "Syscall error: illegal context",
    0x10008: "Syscall error: critical usage",
    0x10009: "Syscall error: illegal number",
    0x20001: "Hardware watchpoint",
    0x20002: "Software watchpoint",
    0x20003: "Hardware breakpoint",
    0x20004: "Software breakpoint",
    0x20005: "Startup failed",
    0x20006: "PRX stop init",
    0x20007: "DTrace breakpoint",
    0x30002: "Undefined instruction exception",
    0x30003: "Prefetch abort exception",
    0x30004: "Data abort exception",
    0x40001: "FPU exception (VFP)",
    0x40002: "FPU exception (NEON)",
    0x60080: "Integer division by zero",
    0x70000: "GPU exception",
})

str_status = defaultdict(str, {
    1: "Running",
    8: "Waiting",
    16: "Not started",
})

str_attr = defaultdict(str, {
    5: "RX",
    6: "RW",
})

reg_names = {
    13: "SP",
    14: "LR",
    15: "PC",
}

core = None


def default_elf():
    env = os.environ.get("MKWV_ELF")
    if env:
        return env
    return str(Path(__file__).resolve().parents[2] / "build-vita" / "vita" / "wiicompiled-vita")


def print_module_info(module):
    iprint(module.name)
    with indent():
        for x, segment in enumerate(module.segments):
            iprint(f"Segment {x + 1}")
            with indent():
                iprint(f"Start: 0x{segment.start:x}")
                iprint(f"Size: 0x{segment.size:x} bytes")
                iprint(f"Attributes: 0x{segment.attr:x} ({str_attr[segment.attr & 0xF]})")
                iprint(f"Alignment: 0x{segment.align:x}")


def print_thread_info(thread, elf=None):
    iprint(thread.name)
    with indent():
        iprint(f"ID: 0x{thread.uid:x}")
        iprint(f"Stop reason: 0x{thread.stop_reason:x} ({str_stop_reason[thread.stop_reason]})")
        iprint(f"Status: 0x{thread.status:x} ({str_status[thread.status]})")
        pc = core.get_address_notation("PC", thread.pc)
        iprint(pc.to_string(elf))
        if not pc.is_located():
            iprint(core.get_address_notation("LR", thread.regs.gpr[14]).to_string(elf))


def main():
    global core

    parser = ArgumentParser(
        description="Parse a PS Vita coredump (.psp2dmp) against an unstripped ELF."
    )
    parser.add_argument("-s", "--stack-size-to-print", dest="stacksize",
                        type=int, help="Number of addresses of the stack to print", metavar="SIZE", default=24)
    parser.add_argument("corefile")
    parser.add_argument("elffile", nargs="?", default=None,
                        help="unstripped ELF (default: build-vita/vita/wiicompiled-vita)")
    args = parser.parse_args()
    stack_size = args.stacksize
    elf_path = args.elffile or default_elf()
    if not os.path.isfile(elf_path):
        raise SystemExit(f"mkwv: ELF not found: {elf_path}")
    if not os.path.isfile(args.corefile):
        raise SystemExit(f"mkwv: coredump not found: {args.corefile}")

    elf = ElfParser(elf_path)
    core = CoreParser(args.corefile)
    iprint("=== THREADS ===")
    crashed = []
    with indent():
        for thread in core.threads:
            if thread.stop_reason != 0:
                crashed.append(thread)
            print_thread_info(thread, elf)
    iprint()
    for thread in crashed:
        iprint(f'=== THREAD "{thread.name}" <0x{thread.uid:x}> CRASHED ({str_stop_reason[thread.stop_reason]}) ===')

        pc = core.get_address_notation("PC", thread.pc)
        pc.print_disas_if_available(elf)
        lr = core.get_address_notation("LR", thread.regs.gpr[14])
        lr.print_disas_if_available(elf)

        iprint("REGISTERS:")
        with indent():
            for x in range(14):
                reg = reg_names.get(x, f"R{x}")
                iprint(f"{reg}: 0x{thread.regs.gpr[x]:x}")
            iprint(pc.to_string(elf))
            iprint(lr.to_string(elf))

        iprint()
        iprint("STACK CONTENTS AROUND SP:")
        with indent():
            sp = thread.regs.gpr[13]
            for x in range(-16, stack_size):
                addr = 4 * x + sp
                data = core.read_vaddr(addr, 4)
                if data:
                    data = u32(data, 0)
                    prefix = "SP =>" if addr == sp else "     "
                    data_notation = core.get_address_notation(f"{prefix} 0x{addr:x}", data)
                    iprint(data_notation.to_string(elf))
    elf.close_addr2line()


if __name__ == "__main__":
    main()
