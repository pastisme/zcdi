# third_party

Vendored dependencies. Each keeps its own licence; the licence text or notice
is in its directory or file headers.

| Directory   | Upstream                                         | Licence           | Local changes |
|-------------|--------------------------------------------------|-------------------|---------------|
| `musashi/`  | [Musashi](https://github.com/kstenerud/Musashi)  | MIT               | Yes, see `musashi/REGENERATE.md` |
| `libchdr/`  | [libchdr](https://github.com/rtissera/libchdr)   | BSD-3-Clause      | No            |
| `libmpeg2/` | [libmpeg2](https://libmpeg2.sourceforge.io/) 0.5.1 release (sha256 `dee22e89…71d4`), the portable C files only | GPL-2.0-or-later | No; `config.h` and `CMakeLists.txt` are added by zcdi, `decode.c`/`header.c` are built by `src/dvc/mpeg2_glue.c` |
| `minimp3/`  | [minimp3](https://github.com/lieff/minimp3) at `ea99364f` (no releases), `minimp3.h` only | CC0-1.0 | No            |
| `libretro/` | [libretro-common](https://github.com/libretro/libretro-common) | MIT | No       |
| `playcdi/`  | CD-i Fan's playcdi, via [CDi_MiSTer](https://github.com/MiSTer-devel/CDi_MiSTer) | LGPL-2.0-or-later | No |
| `doctest/`  | [doctest](https://github.com/doctest/doctest) (tests only) | MIT     | No            |

Local changes are marked inline with `zcdi` comments; keep them when
updating from upstream.

## debugger/third_party

Used only by the debugger (its own CMake project in `debugger/`), not by the core.

| Directory   | Upstream                                         | Licence           | Local changes |
|-------------|--------------------------------------------------|-------------------|---------------|
| `imgui/`    | [Dear ImGui](https://github.com/ocornut/imgui) `v1.92.9b-docking` release tag (docking branch) | MIT | No |
| `imgui_club/` | `imgui_memory_editor.h` from [imgui_club](https://github.com/ocornut/imgui_club) | MIT | No |
| `imgui-filebrowser/` | [imgui-filebrowser](https://github.com/AirGuanZ/imgui-filebrowser) | MIT | No |
| `sokol/`    | [sokol](https://github.com/floooh/sokol)         | zlib              | No            |
