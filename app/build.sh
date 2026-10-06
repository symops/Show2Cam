#!/bin/sh
# Builds the Show2Cam programs on Linux (clang + MinGW-w64).  ARCH=x64 (default) -> app/   ARCH=x86 -> app/x86/
#   Show2Cam.exe        control panel             Show2Cam-Setup.exe  graphical installer
#   s2cinstall.exe      console installer and test tool
#   s2cctl.exe          command line control (what the panel sets)
#   s2clauncher.exe     (x86 only) the package's root Show2Cam-Setup.exe: starts x64\ or x86\Show2Cam-Setup.exe
set -e
cd "$(dirname "$0")"
ARCH=${ARCH:-x64}
if [ "$ARCH" = x86 ]; then TRIPLE=i686-w64-mingw32; TARGET=i686-w64-windows-gnu; O=x86
else TRIPLE=x86_64-w64-mingw32; TARGET=x86_64-w64-windows-gnu; O=.; fi
mkdir -p "$O"
T=${TMPDIR:-/tmp}/s2capp.$$
mkdir -p "$T"
CXX="clang++ --target=$TARGET -std=c++17 -O2 -fno-exceptions -fno-rtti -municode
     -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE -Wall -Wextra -Wno-unused-parameter
     -Wno-missing-field-initializers -isystem /usr/$TRIPLE/include"
for f in applog lang setupcore setupfiles devctl diag s2csetup s2cinstall camdev media audioout sources camcfg s2cpanel cxxrt s2ccamdiag s2cctl; do $CXX -c $f.cpp -o "$T/$f.o"; done
for r in s2csetup s2cinstall s2cpanel s2ccamdiag s2cctl; do $TRIPLE-windres $r.rc -O coff -o "$T/$r.res.o"; done
$TRIPLE-gcc -municode -mwindows -static -s -o "$O/Show2Cam-Setup.exe" "$T/s2csetup.o" "$T/devctl.o" "$T/setupcore.o" \
    "$T/setupfiles.o" "$T/diag.o" "$T/applog.o" "$T/lang.o" "$T/s2csetup.res.o" \
    -lsetupapi -lnewdev -lcfgmgr32 -lcrypt32 -lcomctl32 -lshell32 -lgdi32 -ladvapi32 -luser32 -lole32 -lwevtapi -luuid
$TRIPLE-gcc -municode -mwindows -static -s -o "$O/Show2Cam.exe" "$T/s2cpanel.o" "$T/sources.o" "$T/camcfg.o" "$T/media.o" "$T/audioout.o" \
    "$T/camdev.o" "$T/cxxrt.o" "$T/devctl.o" "$T/setupcore.o" "$T/applog.o" "$T/lang.o" "$T/s2cpanel.res.o" \
    -lmf -lmfplat -lmfreadwrite -lmfuuid -lwindowscodecs -lwinhttp -lcrypt32 -lbcrypt -lsetupapi -lnewdev -lcfgmgr32 -lcomctl32 \
    -lshell32 -lgdi32 -ladvapi32 -luser32 -lole32 -loleaut32 -luuid -lpropsys -lcomdlg32
$TRIPLE-gcc -municode -static -s -o "$O/s2ccamdiag.exe" "$T/s2ccamdiag.o" "$T/camdev.o" "$T/s2ccamdiag.res.o" \
    -lstrmiids -lmf -lmfplat -lmfreadwrite -lmfuuid -lavicap32 -lsetupapi -lshell32 -ladvapi32 -lole32 -loleaut32 -luuid
$TRIPLE-gcc -municode -static -s -o "$O/s2cctl.exe" "$T/s2cctl.o" "$T/camcfg.o" "$T/camdev.o" "$T/media.o" "$T/audioout.o" \
    "$T/cxxrt.o" "$T/devctl.o" "$T/setupcore.o" "$T/applog.o" "$T/lang.o" "$T/s2cctl.res.o" \
    -lwindowscodecs -lsetupapi -lnewdev -lcfgmgr32 -lcrypt32 -lgdi32 -lshell32 -ladvapi32 -luser32 -lole32 -loleaut32 -luuid \
    -lpropsys -lmfplat -lmfuuid -lbcrypt
$TRIPLE-gcc -municode -static -s -o "$O/s2cinstall.exe" "$T/s2cinstall.o" "$T/setupcore.o" "$T/devctl.o" "$T/applog.o" "$T/lang.o" "$T/s2cinstall.res.o" \
    -lsetupapi -lnewdev -lcfgmgr32 -lcrypt32 -lshell32 -ladvapi32 -lole32
if [ "$ARCH" = x86 ]; then
    $CXX -c s2clauncher.cpp -o "$T/s2clauncher.o"
    $TRIPLE-windres s2clauncher.rc -O coff -o "$T/s2clauncher.res.o"
    $TRIPLE-gcc -municode -mwindows -static -s -o "$O/s2clauncher.exe" "$T/s2clauncher.o" "$T/lang.o" "$T/applog.o" \
        "$T/s2clauncher.res.o" -lsetupapi -lshell32 -luser32 -ladvapi32 -lole32
fi
rm -rf "$T"
echo "built ($ARCH, $O): Show2Cam.exe Show2Cam-Setup.exe s2cinstall.exe s2ccamdiag.exe$([ "$ARCH" = x86 ] && echo ' s2clauncher.exe')"
