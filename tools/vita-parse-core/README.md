# vita-parse-core

Python 3 port of [xyzz/vita-parse-core](https://github.com/xyzz/vita-parse-core). It reads a Vita coredump (`.psp2dmp`, gzip or raw) and prints the crashing thread, registers, a disassembly window, and stack words resolved through the unstripped ELF.

The original targets Python 2 and `pyelftools==0.24`. This copy runs on Python 3. Byte strings, `gzip`, and note parsing are updated. A real dump names the module after the ELF (`wiicompiled-vita`, or `eboot.bin`) and the Vita relocates that segment, so a code address is the ELF link address plus the offset from the dump segment's runtime start. The sibling `.velf` supplies the mapped segment size: `vita-elf-create` appends module info past the end of the unstripped ELF.

## Run

From the repo root:

```
scripts/parse-vita-core.sh ux0/data/psp2core-*.psp2dmp
```

The ELF defaults to `build-vita/vita/wiicompiled-vita` (the unstripped link, not the `.velf` or `eboot.bin`). Override it with a second argument or `MKWV_ELF`.

The first run creates `tools/vita-parse-core/.venv` and installs pyelftools. `arm-vita-eabi-objdump` and `arm-vita-eabi-addr2line` come from the Vita SDK (`VITASDK`, default `/usr/local/vitasdk`).

`-s N` is how many stack words to print above the stack pointer (default 24).

Coredumps land in `ux0:data` on the Vita. Copy the `.psp2dmp` off the device; the tool gunzips it itself.

Function names come from the ELF symbol table. File and line numbers need a build with `-g`, which this project does not use, so addr2line reports `??:0` next to the function.
