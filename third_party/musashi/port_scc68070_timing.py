#!/usr/bin/env python3
"""Patches real MAME SCC68070 per-opcode cycle data into the column-5 slot
of the already-regenerated m68kops.c (see REGENERATE.md's "Local
modifications" section for the full recipe and rationale).

This does NOT reimplement MAME's own cycle-derivation arithmetic (the
per-addressing-mode EA-cycle adjustment tables etc.) -- it runs MAME's own,
real, unmodified m68kmake.py generator against MAME's own m68k_in.lst to
get MAME's already-fully-resolved final per-opcode cycle values, then
matches rows against zcdi's own generated table by (mask, match) and
copies the value across directly. This avoids any risk of a hand-transcription
error in re-deriving MAME's arithmetic.

For any zcdi row with no matching MAME row, or where MAME marks the
opcode illegal on the SCC68070 (its 255 sentinel), this keeps zcdi's own
column-1 (68010) value as an explicit, documented fallback -- changing
opcode *legality* is out of scope; only the *cycle count* column is
touched.

Usage:
    python3 port_scc68070_timing.py <path-to-mame-checkout> [--m68kops path]

<path-to-mame-checkout> must contain
src/devices/cpu/m68000/{m68kmake.py,m68k_in.lst}. Requires python3 (no
other dependencies). Re-run this any time m68kops.c is regenerated from
m68k_in.c via the normal recipe -- that normal recipe alone restores
column 5 to the inert 68010-duplicate placeholder from m68k_in.c's own
source table, NOT real SCC68070 data; this script is what makes column 5
real again.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROW_RE = re.compile(
    r'\{(?P<prefix>m68k_op_\S+\s*,\s*(?P<mask>0x[0-9a-f]+)\s*,\s*(?P<match>0x[0-9a-f]+)\s*,\s*\{)'
    r'(?P<cycles>[^}]+)'
    r'(?P<suffix>\}\})'
)

MAME_ROW_RE = re.compile(
    r'\{\s*(?P<value>0x[0-9a-f]+)\s*,\s*(?P<mask>0x[0-9a-f]+)\s*,\s*\{'
    r'(?P<cycles>[^}]+)'
    r'\}\}'
)

ILLEGAL = 255


def run_mame_generator(mame_root: Path) -> str:
    m68kmake = mame_root / 'src/devices/cpu/m68000/m68kmake.py'
    lst = mame_root / 'src/devices/cpu/m68000/m68k_in.lst'
    if not m68kmake.is_file() or not lst.is_file():
        sys.exit(f"error: expected {m68kmake} and {lst} to exist")
    with tempfile.TemporaryDirectory() as td:
        out_h = Path(td) / 'm68kops.h'
        out_cpp = Path(td) / 'm68kops.cpp'
        subprocess.run(
            [sys.executable, str(m68kmake), str(lst), str(out_h), str(out_cpp)],
            check=True, cwd=str(mame_root),
        )
        return out_cpp.read_text()


def parse_mame_table(text: str):
    start = text.index("m68k_opcode_table[] =")
    end = text.index("\n};", start)
    body = text[start:end]
    table = {}
    for m in MAME_ROW_RE.finditer(body):
        mask = int(m.group('mask'), 16)
        value = int(m.group('value'), 16)
        cycles = [int(x.strip()) for x in m.group('cycles').split(',')]
        # cpu_names = '071234fc' -> index 1 is SCC68070 ('7')
        table[(mask, value)] = cycles[1]
    return table


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('mame_root', type=Path)
    ap.add_argument('--m68kops', type=Path,
                     default=Path(__file__).parent / 'm68kops.c')
    args = ap.parse_args()

    mame_text = run_mame_generator(args.mame_root)
    mame_table = parse_mame_table(mame_text)

    zcdi_text = args.m68kops.read_text()
    start = zcdi_text.index("m68k_opcode_handler_table[] =")
    end = zcdi_text.index("\n};", start)

    patched = 0
    fallback_no_match = 0
    fallback_illegal = 0

    def repl(m):
        nonlocal patched, fallback_no_match, fallback_illegal
        mask = int(m.group('mask'), 16)
        match = int(m.group('match'), 16)
        cycles = [c.strip() for c in m.group('cycles').split(',')]
        col1 = int(cycles[1])
        mame_cycle = mame_table.get((mask, match))
        if mame_cycle is None:
            fallback_no_match += 1
            new_val = col1
        elif mame_cycle == ILLEGAL:
            fallback_illegal += 1
            new_val = col1
        else:
            patched += 1
            new_val = mame_cycle
        cycles[5] = f'{new_val:3d}'
        return '{' + m.group('prefix') + ', '.join(cycles) + m.group('suffix')

    body = zcdi_text[start:end]
    new_body = ROW_RE.sub(repl, body)
    total = patched + fallback_no_match + fallback_illegal
    zcdi_text = zcdi_text[:start] + new_body + zcdi_text[end:]
    args.m68kops.write_text(zcdi_text)

    print(f"total rows processed: {total}")
    print(f"patched with real MAME SCC68070 data: {patched}")
    print(f"fallback (no matching MAME row): {fallback_no_match}")
    print(f"fallback (MAME marks illegal on SCC68070): {fallback_illegal}")


if __name__ == '__main__':
    main()
