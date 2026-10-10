# zcdi

![zcdi](zcdi.svg)

[![Build](https://github.com/pastisme/zcdi/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/pastisme/zcdi/actions/workflows/build.yml)
[![License: GPL-3.0-or-later](https://img.shields.io/badge/license-GPL--3.0--or--later-blue)](LICENSE)

Zcdi is a **Philips CD-i (Mono-I / `cdimono1`)** emulator library with built-in **libretro** core support.

Zcdi aims to bridge the final gap between [MAME](https://github.com/mamedev/mame) -> [same_cdi](https://github.com/zach-morris/same_cdi) -> [libretro same_cdi](https://github.com/libretro/same_cdi) and [CDi_MiSTer](https://github.com/MiSTer-devel/CDi_MiSTer) to bring DVC support to software CD-i emulation. Much of this builds on the research of [CD-i Fan](https://www.cdiemu.org/).

The goal of zcdi is to play CD-i discs rather than to be a cycle-exact replica of the hardware. Zcdi only emulates the Mono-I board and the 22ER9141 Digital Video Cartridge. The original hardware can also play CD-DA and Video CD; these are considered bonuses for this project.
Zcdi relies on CDi_MiSTer reverse-engineering work for DVC support.

The project also includes a desktop debugger built on the library; see [debugger/](debugger/README.md).

## Required ROM files

They are loaded from the frontend's system directory (or the current working directory if the frontend provides none).

|md5|file|note|
|-|-|-|
|2969341396aa61e0143dc2351aaa6ef6|cdi200.rom|NTSC|
|ac0d468be366779c9df633be98da250a|cdi220b.rom|PAL|
|53ba3bf7443187a657baa272d01d3771|vmpega.rom|DVC (optional)|

## TODOs

- [ ] Comprehensive title testing.
- [ ] Support for [RetroAchievements](https://retroachievements.org/).
- [ ] Support for Tool Assisted Speed Runs.

## FAQ

**Are all CD-i titles supported?**

It's the goal but they all have to be tested.

**Will other boards be supported?**

Not in the plan, CD-i is first and foremost a CD format, the main purpose is to be able to play them.

**Is zcdi hardware faithful?**

As much as it can.

**same_cdi already supports DVC, why zcdi?**

The development of this project started before DVC support for same_cdi was made public.

**How can I help?**

Open a [compatibility report](https://github.com/pastisme/zcdi/issues/new?template=report_compatibility.yml), or support the project:

[<img src="https://img.shields.io/badge/Ko--fi-Support%20zcdi-FF5E5B?logo=ko-fi&logoColor=white" alt="Support me on Ko-fi" height="30">](https://ko-fi.com/pastisme)

**Is AI used in this project?**

Yes.

## Licence

zcdi is licensed under the GNU General Public License, version 3 or (at your
option) any later version; see [LICENSE](LICENSE). Bundled third-party
components keep their own licences; see [third_party/](third_party/README.md)
and [mamecdi/LICENSE](mamecdi/LICENSE) for the MAME-derived code.
