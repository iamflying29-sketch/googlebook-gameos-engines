# GameOS Engines

These are the build scripts for the engines inside **GameOS**, my open-source gaming app for x86_64 Googlebooks.
GameOS is a SteamOS-style Gaming Mode written in Kotlin. It runs Windows games through its own Wine and Proton, and
console games through emulators, all inside one Android app. The app itself will get its own repository once its
first milestone works.

This repository holds only scripts, patches and GitHub Actions workflows. GitHub's servers do the heavy builds,
and every result is published as a release with a SHA-256 checksum. GameOS downloads its engines from those releases.

## What gets built

| Engine | What it is for | Status |
|---|---|---|
| Runtime glibc | The C library that Wine, Mesa and the emulators run on inside the app | Script and patch ready |
| Mesa Intel Vulkan driver | GameOS's own up-to-date graphics driver for the Googlebook's Intel GPU | Next |
| Wine with Proton's patches | Runs Windows games, including 32-bit ones | Next |
| DXVK and VKD3D-Proton | Direct3D 8 to 12 on Vulkan | Next |

## Why GameOS needs its own glibc

Android protects every app with a system call filter. Ordinary Linux programs use a few old system calls that this
filter blocks (`access`, `mkdir`, `poll`, `set_robust_list` and `rseq`), and the app is then killed. I measured
this on my Googlebook. The patch in `runtime/glibc/patches` makes glibc use the modern equivalents that Android
allows. It also adds a small handler that translates any old call still made directly by a program, so a game
keeps running instead of crashing. Every library is built with 16 KB page alignment, which Android requires for
newer devices, and with x86-64-v3 instructions for speed.

## Building locally

```bash
WORK=$PWD/work OUT=$PWD/out JOBS=6 runtime/glibc/build.sh
```

The script downloads the official glibc release, checks its SHA-256 checksum, applies the patch, builds, and then
refuses to finish if any library is not 16 KB aligned. It never deletes anything; every run uses new dated folders.

## License

GPL-3.0-or-later, the same license GameOS uses. Each engine keeps its own license; glibc is LGPL-2.1-or-later.

I built this with the help of Claude Code.
