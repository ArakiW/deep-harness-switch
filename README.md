# DEEP HARNESS SWITCH

> Nintendo Switch homebrew — an AI-agent / optical-diagnostic terminal built on the console's unusual hardware.

This repository is the **Phase 1 IRS benchmark/probe** for the *DEEP HARNESS SWITCH* project: a minimal `.nro` that initializes the right Joy-Con IR camera and measures its real acquisition timing on physical hardware — before any scanner logic is built.

## What this probe does

- Initializes the right Joy-Con IR camera via `libnx` (`irs.h`).
- Runs the `ImageTransferProcessor` and shows the raw 8-bit grayscale image.
- Cycles through every `IrsImageTransferProcessorFormat` exposed by libnx:
  `320x240`, `160x120`, `80x60`, `40x30`, `20x15`.
- Measures per format:
  - time to first frame
  - sampling interval: average / median / p95 / max
  - acquisition error count
- Toggles the IR LED (`light_target`: all-on ↔ off).
- Saves a benchmark report to the SD card.

## Controls

| Button | Action |
| ------ | ------ |
| `X`    | Cycle image-transfer format |
| `A`    | Toggle IR LED |
| `B`    | Reset timing for the current format |
| `Y`    | Save benchmark report to SD |
| `+`    | Exit |

Benchmark reports are written to:

```text
sdmc:/switch/deep-harness-switch/bench_<format>.log
```

## Building

### devkitPro (recommended / CI)

```bash
dkp-pacman -S --noconfirm --needed \
  switch-sdl2 switch-sdl2_ttf \
  switch-freetype switch-harfbuzz \
  switch-zlib switch-libpng switch-bzip2
make
```

### Windows (clang + ld.lld against a local devkitA64 sysroot)

```powershell
$env:DSH_SWITCH_DEPS = "<dir containing .tools\devkita64-sysroot and .tools\switch-tools-win>"
$env:LLVM_BIN = "C:\Program Files\LLVM\bin"
$env:MSYS2_BIN = "C:\msys64\usr\bin"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File scripts\build.ps1
```

## Why a benchmark first?

The Joy-Con IR camera is not a normal camera. The official
[switchbrew example](https://github.com/switchbrew/switch-examples/blob/master/hid/irsensor/source/main.c)
warns that the maximum image-transfer format has the slowest update rate and
may update only every few seconds. CamNX users have also observed lag at
maximum resolution.

The hidden-camera scanner must therefore be designed as a *measurement
instrument* (low-resolution search + explicit high-resolution verification),
not a smooth camera app. The timings measured by this probe drive those design
decisions. No frame-rate claims are made until measured on the target device.

## What must be tested on physical hardware

This NRO is **not yet validated on a real Switch**. The following need on-device measurement:

- Per-format frame intervals (avg/median/p95) and first-frame latency.
- Whether `irsRunImageTransferProcessor` + `light_target` reliably toggles the IR LED while frames keep flowing.
- Behavior when the right Joy-Con is missing or disconnected.
- Whether the raw image is usable when the Joy-Con is detached.

## Repository layout

```text
source/          main.c (IRS probe)
scripts/         build.ps1 (Windows clang/ld.lld route)
linker/          switch-lld.ld
assets/          embedded font
romfs/           font for the romfs route
```

## References

- [switchbrew libnx `irs.h`](https://github.com/switchbrew/libnx)
- [switchbrew IR sensor example](https://github.com/switchbrew/switch-examples/blob/master/hid/irsensor/source/main.c)
- [Switchbrew HID / IRS documentation](https://switchbrew.org/wiki/HID_services)

Implementation is written from the libnx API and the switchbrew example's
documented API flow. No `All Rights Reserved` third-party source (e.g. CamNX)
is copied or derived.

## License

Project source: **MIT License**.

Embedded font **Noto Sans CJK SC**: **SIL Open Font License 1.1** (see `LICENSE-FONTS.txt`).
