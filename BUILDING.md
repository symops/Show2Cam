# Building Show2Cam from a clean Debian 13

This describes building **Show2Cam** (repo `symops/Show2Cam`) — the kernel-streaming camera driver, the control
panel, the installer and the tools, for 64-bit and 32-bit Windows — from scratch on a fresh Debian 13 ("trixie")
machine. Everything is built on Linux with clang and MinGW-w64: no Windows, Visual Studio or WDK is involved, and the
driver is test-signed with a certificate the build creates itself.

## 1. Install the toolchain and build dependencies

```sh
sudo apt update
sudo apt install -y \
    git clang mingw-w64 gcc make libc6-dev \
    osslsigncode openssl gcab python3 python3-pil \
    ffmpeg libheif-examples libheif-plugin-aomenc
```

Checked with exactly these packages in a clean `debian:trixie` container (clang 19, MinGW-w64 12): a full
`./package.sh` without a single warning. Newer toolchains work as well (Ubuntu 26.04: clang 21, a newer MinGW-w64).

What each part is for:

| Packages | Used for |
|---|---|
| `clang`, `mingw-w64` | the driver and the programs (clang compiles against the MinGW-w64 headers, its DDK headers included; the MinGW `gcc`, `windres`, `ar`, `nm` and `dlltool` link, add resources and make the kernel import libraries) — both `x86_64` and `i686` |
| `gcc`, `make`, `libc6-dev` | `tools/generate-cat-file`, a small host program that writes the driver's security catalog (`.cat`) |
| `osslsigncode`, `openssl` | the test certificates, the signature of `Show2Cam.sys` and of the catalog |
| `gcab` | the cabinet inside the one-file installer |
| `python3`, `python3-pil` | `gen.py` (version, INF), the language tables, the INF variants and the autotest's samples |
| `ffmpeg`, `libheif-examples`, `libheif-plugin-aomenc` | the autotest's sample clips and pictures in the formats Windows cannot write itself (H.264/HEVC/VP8/VP9/AV1, MP3/Vorbis/Opus/AC-3, WMV, AVIF) — made anew on every build, see step 6 |

Optional, only for `./check_inf.sh` (Microsoft's INF checker, step 7): `sudo apt install -y wine`.

Debian's `ffmpeg` already has every encoder the samples need (libx264, libx265, libvpx, libaom, libmp3lame,
libvorbis, libopus). HEIC is written by the sample script itself (one HEVC frame from ffmpeg put into an HEIF
container), because `heif-enc` has no HEVC encoder on Debian.

## 2. Clone the repository

```sh
git clone git@github.com:symops/Show2Cam.git
cd Show2Cam
```

`third_party/libyuv` (the frame scaling, BSD licence) is in the repository: there is nothing else to fetch.

## 3. The test signing keys

The driver has no Microsoft signature: Windows loads it only in **test signing mode** and only when it trusts the
certificate it was signed with. `driver/mingw/build.sh` creates that certificate on the first build when
`driver/mingw/testcert/` is missing:

- `root.pem` / `root.key` — "Show2Cam Test Root CA", valid 10 years; the installer puts it into the machine's
  *Trusted Root* store (`Show2Cam.cer` in the package),
- `cert.pem` / `key.pem` — "Show2Cam Test Signing", issued by that root, which signs `Show2Cam.sys` and the catalog;
  the installer puts it into *Trusted Publishers* (`Show2Cam-Publisher.cer`),
- `chain.pem` — both, for `osslsigncode`.

**Never publish `driver/mingw/testcert/`** (it is in `.gitignore`): every machine the installer ran on trusts that
root for anything signed with it. **Keep it** between builds: an update signed with the same root installs over the
old version without a new certificate prompt. A build machine without the folder makes new keys; installers built
from them still work (they add their own root), the machines just trust one more test root.

## 4. Build

```sh
./package.sh
```

One command builds and packages everything; it takes a few minutes. In order:

1. `gen.py` — a new version **1.0.\<day of year\>.\<minute of day\>** into `driver/version.h` (programs, version
   resources, installer) and the INF's `DriverVer`. Every build must have a new one: Windows identifies a driver
   package by its INF and treats an INF with an unchanged `DriverVer` as "already imported".
2. `app/lang/gen_lang.py` — the 17 interface languages into `app/lang_table.inc`.
3. `driver/mingw/build.sh` for x64 and x86 — `Show2Cam.sys`: compiled against the MinGW DDK headers, linked with
   import libraries generated from what the objects import (`ks.sys`, `ntoskrnl.exe`, `hal.dll` on x86), the PE
   header fixed for a kernel driver (`pefix.py`), test-signed.
4. `app/build.sh` for x64 and x86 — the programs (step 5), libyuv as a static library, and the autotest's samples
   (step 6) on the x64 pass.
5. `tools/generate-cat-file` — the catalog of `Show2Cam.inf` + `Show2Cam.sys` for each architecture, test-signed and
   checked (`osslsigncode verify`); Windows refuses a driver package without a catalog (`0xE000022F`), also in test
   signing mode.
6. The two packages and their one-file installers (step 5 below), and the link maps (step 8).

`set -e` stops at the first error. Warnings are not expected: a build is clean when
`grep -iE "error|warning:"` finds nothing in its output.

## 5. What comes out

| Path | Contents |
|---|---|
| `dist/Show2Cam-Setup.exe` | **the release**, one file: the 32-bit launcher with the package below appended as a cabinet; it unpacks itself to a temporary folder, starts the installer of this Windows' bitness and cleans up |
| `dist/Show2Cam/` | the same unpacked: `Show2Cam-Setup.exe` (launcher) and `x64\`, `x86\` — `Show2Cam-Setup.exe` (installer), `Show2Cam.exe` (control panel), `Show2Cam.sys`, `Show2Cam.inf`, `Show2Cam.cat`, `Show2Cam.cer`, `Show2Cam-Publisher.cer` |
| `dist/Show2Cam-Debug-Setup.exe`, `dist/Show2Cam-Debug/` | **for testing only**: the same build plus `uninstall.cmd`, `s2cinstall.exe` (console installer), `s2cctl.exe` (command line control), `s2cautotest.exe` (autotest) and `s2ccamdiag.exe` / `s2ccamdiag32.exe` (how programs see the cameras) |

Give users the release. The debug package installs the same product plus the tools; the release installed over it
removes them again (the installer deletes a file of its list that its package does not have).

The programs are 32-bit and 64-bit Windows builds of the same source; the launcher is 32-bit so that it runs on both.
Windows on ARM is not supported.

## 6. The autotest's samples

`app/build.sh` runs `tools/make_autotest_media.py` on every x64 build: pictures (WebP, HEIC, AVIF, ICO) and clips
(about 20 container / codec combinations) in formats Windows cannot encode itself, with new random colours each time.
They go to `app/autotest-media/` and `app/autotest_media.rc` (both ignored by git) and are built into
`s2cautotest.exe` only. The x86 pass reuses the same set. Broken files are **not** among them: the autotest makes
those itself when it starts.

If a sample tool is missing, install it (step 1) rather than skip the samples: the autotest would then not test
those formats.

## 7. Check the INF (optional)

```sh
./check_inf.sh
```

runs Microsoft's **InfVerif** under Wine on the packaged INF (basic, `/h` and `/w` by default; `sudo apt install -y
wine`). InfVerif is not redistributable: take `infverif.exe` from the WDK NuGet package `microsoft.windows.wdk.x64`
(`c/tools/<version>/x64/infverif.exe`) and put it into `tools/infverif/`. Expected: all three pass.

## 8. Link maps

`app/*.map` (x64) and `app/x86/*.map` are copied to `maps/<version>/` (ignored by git). A crash report names a module
and an offset (`CRASH: exception 0xC0000005 at ntdll.dll+0xFA7D ...`, plus a minidump in
`C:\ProgramData\Show2Cam\logs\`); the map of that version tells which function it was in. Keep the maps of every
version you hand out.

## 9. Install for testing

On the test machine (a VM is best):

1. **Secure Boot off** (VMware: VM → Settings → Options → Advanced → clear "Enable UEFI Secure Boot"; Hyper-V:
   Security → clear "Enable Secure Boot"). Windows does not load test-signed drivers with it.
2. Run `Show2Cam-Setup.exe` (or `Show2Cam-Debug-Setup.exe`). If test signing is off, click **Enable test mode**,
   restart, run it again, then **Install**.

Without a window, for scripts (administrator):

```bat
Show2Cam-Setup.exe /install      & rem exit code: 0 works, 1 failed, 2 restart needed,
                                 & rem 3 cannot install here (test signing off / Secure Boot on), 4 installed but not working
Show2Cam-Setup.exe /remove       & rem exit code: 0 removed, 1 failed, 2 restart needed
Show2Cam-Setup.exe /install /dir "D:\Tools"   & rem into D:\Tools\Show2Cam (as "Browse..."; an installation stays where it is)
```

The installed product appears in Windows' **Apps / Programs and Features**; its Uninstall opens the installer.
Logs: `C:\ProgramData\Show2Cam\logs\` (`setup.log`, `panel.log`, `driver.log`, `events.log`, and in the debug
package `install.log`, `ctl.log`, `autotest.log`, `camdiag-*.log`).

## 10. Run the autotest

From the **debug** package, in an administrator command prompt on the test machine:

```bat
"C:\Program Files\Show2Cam\s2cautotest.exe" 120            & rem minutes (default 10); --seed N repeats a run
```

It closes the control panel, remembers the cameras, their names, formats and settings, runs random actions for the
given time and restores everything at the end (also after Ctrl+C). Every check is logged as PASS / FAIL / WARN to
`autotest.log`; the exit code is 1 if anything failed. It can run at the same time as Speak2Mic's autotest; it then
plays clips without sound and tells camera interruptions caused by Speak2Mic's device restarts from its own.

## 11. Code checks

What the code is checked with besides the build, and how to repeat it (all on Linux; the results of the last run: no
compiler warning, CodeQL must-fix clean).

1. **Compiler warnings.** `./package.sh` builds everything with `-Wall -Wextra`, x64 and x86; a clean build prints no
   `warning:` (see step 4).
2. **Clang Static Analyzer** (the programs): for every `app/*.cpp`, with the build's flags,
   ```sh
   cd app
   for f in *.cpp; do
     clang++ --target=x86_64-w64-windows-gnu -std=c++17 -fno-exceptions -fno-rtti -municode -D_WIN32_WINNT=0x0A00 \
       -DWINVER=0x0A00 -DUNICODE -D_UNICODE -isystem /usr/x86_64-w64-mingw32/include -I../third_party/libyuv/include \
       --analyze -Xclang -analyzer-output=text -o /dev/null "$f"
   done
   ```
   The findings left are false positives (memory freed by the window a message is posted to, a window's state freed in
   `WM_NCDESTROY`).
3. **CodeQL with Microsoft's driver rules** - the static analysis of the Windows Hardware Compatibility Program
   ("Static Tools Logo"): CodeQL CLI 2.25.5 (github.com/github/codeql-cli-binaries) and the query packs
   `microsoft/windows-drivers@1.10.0` + `microsoft/cpp-queries@0.0.5` (`codeql pack download ...`). The build command goes
   in a script (CodeQL splits `--command` on spaces):
   ```sh
   printf '#!/bin/sh\nARCH=x64 exec sh driver/mingw/build.sh\n' > /tmp/drv.sh && chmod +x /tmp/drv.sh
   codeql database create /tmp/db --overwrite --language=cpp --source-root=. --command=/tmp/drv.sh
   codeql database analyze /tmp/db microsoft/windows-drivers:windows-driver-suites/mustfix.qls \
     --format=sarifv2.1.0 --output=show2cam-mustfix.sarif
   ```
   Expected: no result for `mustfix.qls`; `recommended.qls` reports only false positives ("uninitialized padding" after `ExAllocatePool2`, which zeroes the memory).
4. **InfVerif**: `./check_inf.sh` (step 7).

Not run here: Static Driver Verifier and Code Analysis (`/analyze`) need Windows with the WDK and Visual Studio; the
HLK tests need a physical test computer.

## Author

Symo &lt;symops@gmail.com&gt;
