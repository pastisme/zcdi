# Regenerating Musashi's opcode tables

`m68kops.c` and `m68kops.h` in this directory are **generated** and **committed**.
They are a pure function of `m68k_in.c` (the generator ignores `m68kconf.h` and
emits portable C), so they are platform-independent and only need regenerating
when the vendored Musashi version changes (i.e. when `m68k_in.c` changes).

To regenerate (host compiler, run once):

    cc -O2 -o m68kmake m68kmake.c
    ./m68kmake . m68k_in.c        # writes m68kops.h and m68kops.c
    rm -f m68kmake

`m68kmake.c` and `m68k_in.c` are kept in the tree for this purpose but are NOT
part of the build.

**Second step, required since the SCC68070 timing port (below)**: the
recipe above alone leaves the SCC68070 cycle column (column index 5) as an
inert placeholder duplicating 68010's value — real MAME data is not stored
in `m68k_in.c` itself (see that entry for why) and must be re-injected
by running:

    python3 port_scc68070_timing.py /path/to/a/mame/checkout

immediately afterward, against the *same* `m68kops.c` the step above just
wrote. Both steps together are the full, real regeneration recipe now.

## Local modifications

Vendored Musashi is otherwise unmodified except for the following real
(non-build-flag) behavioral changes. The opcode-handler change below is in
`m68k_in.c`, propagated into `m68kops.c`/`.h` via the regeneration steps
above; the exception-frame change is directly in the hand-written
`m68kcpu.h` (not part of the generated opcode tables, so no regeneration is
needed for it):

- **`MOVE FROM SR` treated as unprivileged on `CPU_TYPE_SCC070`**, in both
  addressing-mode variants of the `move, 16, frs` opcode handler. Upstream
  Musashi's own `CPU_TYPE_IS_000(CPU_TYPE) || FLAG_S` check treats the
  SCC68070 like a 68010-and-later part (privileged), but two independent
  Philips SCC68070 datasheets (November 1990 and April 1993 "Product
  specification" editions) state, verbatim: "The SCC68070 is completely code
  compatible with the 68000 ... This applies to both source and object
  code." `MOVE FROM SR` is unprivileged on the real 68000 and only became
  privileged starting with the 68010 — real, disc-authored CD-i game code
  (Lost Eden's `cdi_edstart` module) executes it in user mode and expects
  that to succeed. Independently corroborated by Stovent's CeDImu, a
  from-scratch SCC68070 core unrelated to Musashi/MAME: its `MOVEfSR()`
  (`cores/SCC68070/InstructionSet.cpp`) has no privilege check at all, while
  the adjacent `MOVESR()` (MOVE TO SR) explicitly raises
  `PrivilegeViolation` when not supervisor — sourced from the CD-i Green
  Book itself (Chapter VI.2.2.2), not just the general 68000/68010
  datasheets. Lost Eden depends on it.

- **Real SCC68070 Format 15 (17-word) bus/address-error exception frame,
  with the real fault address**, in `m68kcpu.h`: added
  `m68ki_stack_frame_1111()` (ported verbatim from MAME's own
  `src/devices/cpu/m68000/m68kcpu.h`, current tree as of 2026-08-24) and
  wired it into `m68ki_exception_bus_error()`/`m68ki_exception_address_error()`
  for `CPU_TYPE == CPU_TYPE_SCC070`. Upstream Musashi never modelled the
  SCC68070's own frame shape at all — it fell through to the 68010's Format
  8 (29-word) frame with a hardcoded zero fault address, which is both the
  wrong shape and the wrong content for real SCC68070 silicon. Confirmed
  live-active (not just theoretical) via a real BIOS boot trace across three
  canary titles (Lost Eden, The 7th Guest, Lucky Luke): every one raises
  exactly one real address error during early boot, landing in
  `cdi220b.rom`'s own blanket panic-trampoline handler, which reads and prints the fault
  address straight off this exact frame. The address-error branch also
  faithfully replicates a real, documented 68010/68070 hardware quirk (also
  present in MAME's own `m68ki_exception_address_error()`): the frame's
  VECTOR OFFSET field is hardcoded to the BUS ERROR vector's offset even
  when processing an actual address error — `m68ki_jump_vector` still
  correctly vectors through the real ADDRESS ERROR table entry (vector 3)
  regardless of what the frame itself displays. The bus-error branch is
  reached through zcdi's `m68k_pulse_bus_error_at()` for undecoded
  addresses. Covered by `tests/unit/cpu/exception_frame_test.cpp`.

- **Real, per-opcode SCC68070 cycle-timing data**, replacing a full alias
  onto CPU_TYPE_68010's timing (`m68kcpu.c`'s `M68K_CPU_TYPE_SCC68070` case
  used to call `m68k_set_cpu_type(M68K_CPU_TYPE_68010)` and share its exact
  `m68ki_cycles[1]` pointer — no SCC68070-specific timing existed anywhere
  in this codebase before this change). Structural changes:
  - `m68k_in.c`: `NUM_CPU_TYPES` 5→6; every one of the 518 data rows gets a
    6th mode+cycle column (a mechanical, scripted transform — see git
    history / the original port session for the exact regex — duplicating
    column index 1 as an inert placeholder; **this placeholder is never
    actually used** once `port_scc68070_timing.py` has run, see below).
  - `m68kmake.c`: `enum` gets `CPU_TYPE_SCC070` appended *after*
    `CPU_TYPE_040` (not inserted earlier), specifically so every existing
    ordinal comparison in `get_oper_cycles()` (`cpu_type < CPU_TYPE_020`,
    `== CPU_TYPE_010`, `== CPU_TYPE_000`) keeps meaning exactly what it
    always has for the original 5 types. `get_oper_cycles()` itself is
    otherwise **untouched** — deliberately: hand-authoring a 6th column
    into its EA-mode-adjustment tables (`g_ea_cycle_table` and the
    `moves`/`clr`/`jmp`/`jsr`/`lea`/`pea`/`movem`-specific tables) would
    mean re-deriving MAME's own per-addressing-mode cycle arithmetic by
    hand — a real transcription-error risk. Because nothing reads
    `m68ki_cycles[5]` until `m68kcpu.c`'s SCC68070 case is wired to it
    (below), whatever intermediate value this leaves in column 5 during a
    plain regen is inert/unobserved.
  - `port_scc68070_timing.py` (new, committed, re-runnable): runs MAME's
    own real, unmodified `m68kmake.py` against MAME's own real
    `m68k_in.lst` (validated this session against commit
    `63e3a8f9009f541a6438a8f6c6c3a52c669b4de6`, 2026-08-24) to get MAME's
    already-fully-resolved final per-`(mask,match)` cycle values — not a
    hand-reimplementation of its derivation logic — then patches column 5
    of the *already-generated* `m68kops.c` directly: for each zcdi row, if
    a MAME row matches by `(mask, match)` and MAME's SCC68070 cycle isn't
    its `255` ("illegal on this CPU type") sentinel, copies that real
    value in; otherwise falls back to that row's own column-1 (68010)
    value, read directly from the same table (guaranteed correct, no
    dependency on the inert `m68k_in.c` placeholder). Verified via direct
    row-by-row comparison: 1964/1967 zcdi rows (99.85%) find a MAME match;
    of the 1676 with valid (non-illegal) MAME data, the overwhelming
    majority differ from the old 68010-aliased value (e.g. `MOVEQ` = 7
    cycles on real SCC68070 vs 4 on 68000/68010; taken `Bcc` = 13 vs 10;
    `BSR` = 17 vs 18) — a systemic, not edge-case, timing gap. 288 rows
    fall back because MAME marks them illegal on the SCC68070 (real
    020+-only opcodes zcdi's own 68010-derived decode table still
    dispatches — changing *legality* is out of scope, only the *cycle
    count* column is touched); 3 fall back because no MAME row matches at
    all (FPU/coprocessor dispatch edge cases already stubbed to 0 cycles).
  - `m68kcpu.c`'s `M68K_CPU_TYPE_SCC68070` case: no longer aliases 68010 —
    sets `CYC_INSTRUCTION = m68ki_cycles[5]`,
    `CYC_EXCEPTION = m68ki_exception_cycle_table[5]` (own dedicated slots),
    `CPU_SR_MASK = 0xa71f`, and the small flat per-type correction
    constants (`CYC_BCC_NOTAKE_B/W`, `CYC_DBCC_F_NOEXP/EXP`,
    `CYC_SCC_R_TRUE`, `CYC_MOVEM_W/L`, `CYC_SHIFT`, `CYC_RESET`) — all
    verbatim transcriptions of MAME's own real `init_cpu_scc68070()`
    (`src/devices/cpu/m68000/m68kcpu.cpp`).
  - `m68ki_exception_cycle_table[5][256]` → `[6][256]`, new row 5 originally
    a placeholder duplicate of row 1 (68010) for every entry. Six entries
    (indices 2/3 Bus/Address Error, 4/8/9 Illegal Instruction/Privilege
    Violation/Trace, 25-31 the seven Interrupt Autovector levels) were later
    replaced with real values sourced directly from the Philips SCC68070
    datasheet's own Table 19 ("Exception processing clock periods":
    158/158/55/55/55/65 respectively) — not from MAME, since MAME's own
    SCC68070 core has no per-exception-type cycle table of its own to port
    from for this one. Every other entry in row 5 (Reset, CHK, TRAPV,
    Divide by Zero, TRAP #n, Uninitialized/Spurious Interrupt, Format Error)
    remains the original 68010-duplicate placeholder, pending a real source
    Table 19 doesn't cover — this is a plain hand-edit in `m68kcpu.c`, not
    something `port_scc68070_timing.py` (opcode timing only) or any
    regeneration step touches or would revert.
  - Verified: `tests/unit/cpu/` (exception_frame_test.cpp, opcode_timing_test.cpp) spot-checks concrete known
    values (`MOVEQ`=7, `BRA.S`=13, `BSR.S`=17); a before/after screenshot
    comparison across 6 canary titles (Lost Eden, The 7th Guest, Lucky
    Luke, Chaos Control, Apprentice, Zelda — 6000 frames each) was
    byte-identical in every case, and all 6 completed without hang/crash.
