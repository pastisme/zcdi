A desktop debugger for zcdi, built with [sokol](https://github.com/floooh/sokol) and [Dear ImGui](https://github.com/ocornut/imgui). It runs the zcdi library directly, without libretro, so it is also the simplest example of a frontend built on the library.

## Build

```sh
cmake -S debugger -B debugger/build -DCMAKE_BUILD_TYPE=Release
cmake --build debugger/build --config Release
```

## Run

```sh
debugger/build/zcdi_dbg bios=<path_to_bios> disc=<path_to_disc>
```

| Argument | Description |
|-|-|
| `bios=<file>` | BIOS ROM; `cdi220b.rom` starts PAL, others NTSC (switchable in Options) |
| `disc=<file>` | Disc image (.cue, .chd, ...) |

Both are optional: File > Open BIOS and File > Open disc pick them in the debugger, or drop a file on the window: a disc image, a BIOS `.rom`, or `vmpega.rom` for the DVC cartridge (otherwise looked for beside the BIOS). The last BIOS and recent discs are remembered, and the last BIOS is used when `bios=` is not given.

Options > Auto Play starts a CD-i disc without the player's menu, like the libretro core's option. It is saved and takes effect at the next power cycle.

Help > Controls lists every key.

## Features

- **Execution:** run/pause (F5), step into (F11), step over (F10, also over OS-9 TRAP calls), step out (Shift+F11), step to the next field line (F7), run to the end of the frame (F6), run to cursor.
- **Breakpoints:** on addresses, on OS-9 system calls, and on exception vectors (bus/address error, illegal instruction, ...).
- **CPU:** registers (editable while paused), a disassembly that shows OS-9 call names, the stack, and a memory editor (writable while paused).
- **Trace:** the last 65536 instructions and the last exceptions/IRQs, with a per-field timeline.
- **MCD212:** registers, ICA/DCA display lists, CLUT, and single-plane previews.
- **Devices:** CDIC, VMPEG/DVC, SCC68070 and SLAVE state.
- **Rendering and input:** the emulated screen, keyboard pointer control, and mouse capture (F8).
- **Other:** in-memory quick save/load (F2/F4), fullscreen rendering (Alt+Enter toggles, Esc also leaves), dockable windows whose layout persists.

Stopping inside a frame relies on Musashi's per-instruction hook, which only this project's build enables.
