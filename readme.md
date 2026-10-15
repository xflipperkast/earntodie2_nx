<div align="center">

<img src="icon.jpg" alt="earntodie2_nx" width="160">

# earntodie2_nx

**Earn to Die 2 on Nintendo Switch**

An unofficial Nintendo Switch port of the Android version of  
**Earn to Die 2 — 1.5.6**

**By xFlippy**

[![Switch](https://img.shields.io/badge/Nintendo_Switch-Homebrew-E60012?style=for-the-badge&logo=nintendoswitch&logoColor=white)](#)
[![Ko-fi](https://img.shields.io/badge/Support_on_Ko--fi-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/flippyy)

</div>

---

## About

`earntodie2_nx` is an experimental Nintendo Switch port targeting **Earn to Die 2 Android 1.5.6 (ARM64)**.

It provides the compatibility layer that loads the Android package under Horizon OS.

> Supply your own legally obtained XAPK. The game package, native libraries and gameplay data are not included in the source or NRO ZIPs. The displayed launcher icon is extracted from the Android release.

---

## Controls

The current input bridge provides touchscreen support


### During a Run

Use the game's on-screen controls through the touchscreen. Dedicated controller mappings for driving

---

## Build

### Requirements

- [devkitPro](https://devkitpro.org/), devkitA64 and libnx
- Switch portlibs: Mesa/libdrm_nouveau, libvorbis/libogg, FreeType, curl, Mbed TLS and zlib
- Linux or WSL, GNU Make, Bash, Python 3.11+, curl, tar and patch
- A host C compiler and zlib headers for `make check`
- Your own legally obtained **Earn to Die 2 Android 1.5.6 ARM64 XAPK** to run the game

### Compile

Open the source checkout:

```bash
cd earntodie2_nx
```

Configure your devkitPro environment:

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=$DEVKITPRO/devkitA64
export PATH=$DEVKITA64/bin:$DEVKITPRO/tools/bin:$PATH
```

Build the homebrew:

```bash
make -j$(nproc)
```

To rebuild from scratch:

```bash
make clean
make -j$(nproc)
```

The output is **`earntodie2_nx.nro`**
---

## Running

Place these two files together on your SD card:

```text
sd:/switch/earntodie2_nx/
├── earntodie2_nx.nro
└── earn2die.xapk
```

Rename your original XAPK to **`earn2die.xapk`**. Keep the base APK and ARM64/resource splits inside it; do not unpack the archive or modify the game libraries.

**Use application/title takeover mode.** Hold **R** while opening an installed game to enter hbmenu, then launch **Earn to Die 2**. Applet mode is not the supported launch mode.

The port creates `saves/`, `cache/`, `config/` and `logs/` beside the two files as needed. Writable game data stays under `sd:/switch/earntodie2_nx/`. Replace only the NRO when updating; keep the XAPK and save folders.

---

## Support

You can support development through Ko-fi:

<div align="center">

[![Support me on Ko-fi](https://img.shields.io/badge/Support_me_on_Ko--fi-FF5E5B?style=for-the-badge&logo=ko-fi&logoColor=white)](https://ko-fi.com/flippyy)

</div>

---

## Disclaimer

This is an unofficial fan project. It is not affiliated with or endorsed by **Not Doppler** or **Toffee Games**.

Earn to Die 2, its trademarks, launcher artwork, audio and game assets belong to their respective rightsholders. The homebrew menu author field identifies the port maintainer, **xFlippy**.
---

## Contribution

Bug reports, performance improvements and documented pull requests are welcome. Include your game version, launch mode, reproduction steps and relevant log output. Do not attach game packages, native libraries, personal save data or credentials to public issues.
