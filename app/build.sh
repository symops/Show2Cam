#!/bin/sh
# Builds the Show2Cam programs on Linux (clang + MinGW-w64).  ARCH=x64 (default) -> app/   ARCH=x86 -> app/x86/
set -e
cd "$(dirname "$0")"
ARCH=${ARCH:-x64}
if [ "$ARCH" = x86 ]; then TRIPLE=i686-w64-mingw32; O=x86; else TRIPLE=x86_64-w64-mingw32; O=.; fi
mkdir -p "$O"
T=${TMPDIR:-/tmp}/s2capp.$$
mkdir -p "$T"
CXX="clang++ --target=$TRIPLE -std=c++17 -O2 -fno-exceptions -fno-rtti -municode
     -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DUNICODE -D_UNICODE -Wall -Wextra -Wno-unused-parameter
     -Wno-missing-field-initializers -isystem /usr/$TRIPLE/include"
$CXX -c s2cinstall.cpp -o "$T/s2cinstall.o"
$TRIPLE-windres s2cinstall.rc -O coff -o "$T/s2cinstall.res.o"
$TRIPLE-gcc -municode -static -s -o "$O/s2cinstall.exe" "$T/s2cinstall.o" "$T/s2cinstall.res.o" \
    -lsetupapi -lnewdev -lcfgmgr32 -lcrypt32 -lshell32 -ladvapi32 -lole32
rm -rf "$T"
echo "built ($ARCH, $O): s2cinstall.exe"
