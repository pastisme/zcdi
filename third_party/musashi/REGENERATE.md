# Regenerating Musashi's opcode tables

`m68kops.c` and `m68kops.h` are generated from `m68k_in.c` and committed. Regenerate them only when `m68k_in.c` changes. `m68kmake.c` and `m68k_in.c` are not part of the build.

    cc -O2 -o m68kmake m68kmake.c
    ./m68kmake . m68k_in.c        # writes m68kops.h and m68kops.c
    rm -f m68kmake
    python3 port_scc68070_timing.py /path/to/mame

The last step fills the SCC68070 cycle column (index 5) of the new `m68kops.c` with MAME's values. Without it, the column repeats the 68010 values.

## Local modifications

- **`MOVE from SR` is unprivileged on `CPU_TYPE_SCC070`** (`m68k_in.c`). The SCC68070 is 68000 code compatible, and CD-i titles run this instruction in user mode.
- **SCC68070 bus and address error frame** (`m68kcpu.h`). `m68ki_stack_frame_1111()` is ported from MAME. It produces the 17-word Format 15 frame with the fault address.
- **SCC68070 cycle timing:**
  - `m68k_in.c` and `m68kmake.c` have a 6th CPU type column; `CPU_TYPE_SCC070` is last in the enum.
  - `port_scc68070_timing.py` copies MAME's per-opcode cycle counts into that column. Rows MAME marks illegal keep the 68010 value.
  - `m68kcpu.c` has its own SCC68070 setup, ported from MAME's `init_cpu_scc68070()`.
  - The exception cycle table entries for bus and address error, illegal instruction, privilege violation, trace and interrupt autovectors come from Table 19 of the Philips SCC68070 datasheet. The other entries keep the 68010 values.
