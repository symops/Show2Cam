#!/bin/sh
# Builds Show2Cam.sys on Linux with clang + MinGW-w64 (no WDK) and test-signs it.
#   ARCH=x64 (default) -> driver/mingw/out/      ARCH=x86 -> driver/mingw/out-x86/
# The import libraries (ks.sys, ntoskrnl.exe, and hal.dll on x86) are generated from what the objects import.
set -e
cd "$(dirname "$0")"
ARCH=${ARCH:-x64}
DRV=..
OBJ=${TMPDIR:-/tmp}/s2cdrv.$$
if [ "$ARCH" = x86 ]; then
    OUT=out-x86
    TRIPLE=i686-w64-mingw32
    ARCHFLAGS="--target=i686-w64-windows-gnu -D_X86_=1 -Di386=1"
    ENTRY=_DriverEntry@8
    BASE=0x10000
else
    OUT=out
    TRIPLE=x86_64-w64-mingw32
    ARCHFLAGS="--target=x86_64-w64-windows-gnu -mno-red-zone -D_AMD64_ -D_WIN64"
    ENTRY=DriverEntry
    BASE=0x140000000
fi
MINGW=/usr/$TRIPLE/include
mkdir -p "$OBJ" "$OUT"

CXXFLAGS="$ARCHFLAGS -std=c++17 -O2 -fms-extensions -fno-exceptions -fno-rtti
  -fno-stack-protector -fno-threadsafe-statics -fno-use-cxa-atexit
  -DNTDDI_VERSION=0x0A00000C -D_WIN32_WINNT=0x0A00 -D_KERNEL_MODE=1
  -include $DRV/check_prelude.h -isystem $MINGW/ddk -isystem $MINGW -I$DRV
  -Wall -Wno-unknown-pragmas -Wno-ignored-attributes -Wno-microsoft-anon-tag -Wno-nonportable-include-path
  -Wno-missing-braces -Wno-missing-field-initializers"

for f in device camera log; do
    clang++ $CXXFLAGS -c $DRV/$f.cpp -o "$OBJ/$f.o"
done
$TRIPLE-windres -c 65001 -I$DRV $DRV/Show2Cam.rc -O coff -o "$OBJ/resources.o"

# One .def per module from the undefined symbols: __imp_X (dllimport) and the C runtime functions the compiler
# calls directly. On x86 the names carry the calling convention (_Name@N stdcall, @Name@N fastcall, _Name cdecl);
# dlltool -k binds them to the undecorated export names.
$TRIPLE-nm -u "$OBJ"/*.o | sed -n 's/^ *U //p' | sort -u | ARCH=$ARCH OBJ=$OBJ python3 -c "
import os, sys
x86 = os.environ['ARCH'] == 'x86'
crt = {'memset', 'memcpy', 'memmove', '_snprintf', '_vsnprintf', '_vsnwprintf', '_snwprintf', 'strlen', 'wcslen'}
hal = {'KfAcquireSpinLock', 'KfReleaseSpinLock', 'KeQueryPerformanceCounter', 'KeGetCurrentIrql', 'KfRaiseIrql',
       'KfLowerIrql', 'ExAcquireFastMutex', 'ExReleaseFastMutex', 'ExTryToAcquireFastMutex'}
defs = {'ks.sys': [], 'ntoskrnl.exe': [], 'hal.dll': []}
for sym in sys.stdin.read().split():
    if sym.startswith('__imp_'): sym = sym[6:]
    else:
        bare = sym[1:] if x86 and sym.startswith('_') else sym
        if bare not in crt: continue                       # libgcc helpers and the like
    if x86:
        if sym.startswith('@'): name, decorated = sym[1:].split('@')[0], sym     # fastcall
        else: decorated = sym[1:]; name = decorated.split('@')[0]                 # stdcall / cdecl
    else:
        name = decorated = sym
    dll = 'ks.sys' if name.startswith('Ks') else 'hal.dll' if x86 and name in hal else 'ntoskrnl.exe'
    defs[dll].append(decorated)
for dll, names in defs.items():
    with open(os.environ['OBJ'] + '/' + dll.split('.')[0] + '.def', 'w') as f:
        f.write('LIBRARY ' + dll + '\n' + 'EXPORTS\n' + ''.join(n + '\n' for n in sorted(set(names))))
"
K=; [ "$ARCH" = x86 ] && K=-k
$TRIPLE-dlltool $K -d "$OBJ/ks.def" -D ks.sys -l "$OBJ/libks_s2c.a"
$TRIPLE-dlltool $K -d "$OBJ/ntoskrnl.def" -D ntoskrnl.exe -l "$OBJ/libntoskrnl_s2c.a"
LIBS="-lks_s2c -lntoskrnl_s2c"
if [ "$ARCH" = x86 ]; then
    $TRIPLE-dlltool -k -d "$OBJ/hal.def" -D hal.dll -l "$OBJ/libhal_s2c.a"
    LIBS="$LIBS -lhal_s2c $($TRIPLE-gcc -print-libgcc-file-name)"     # 64-bit division
fi

$TRIPLE-gcc -nostdlib -shared \
    -Wl,--subsystem,native -Wl,--entry,$ENTRY -Wl,--image-base,$BASE \
    -Wl,--dynamicbase -Wl,--nxcompat \
    -Wl,--file-alignment,0x200 -Wl,--section-alignment,0x1000 -Wl,--no-insert-timestamp \
    -Wl,--exclude-all-symbols -Wl,--major-subsystem-version,10 -Wl,--minor-subsystem-version,0 \
    -Wl,--major-os-version,10 -Wl,--minor-os-version,0 \
    -o "$OBJ/Show2Cam.sys" "$OBJ"/*.o -L"$OBJ" $LIBS

python3 pefix.py "$OBJ/Show2Cam.sys"

# Test certificates (created once, reused so Windows keeps trusting the same publisher; never publish them):
#   root.pem  - "Show2Cam Test Root CA" (CA:TRUE)      -> machine "Root" store      (Show2Cam.cer)
#   cert.pem  - "Show2Cam Test Signing" (codeSigning)  -> "TrustedPublisher" store  (Show2Cam-Publisher.cer)
CERT=testcert
if [ ! -f $CERT/root.pem ] || [ ! -f $CERT/cert.pem ]; then
    rm -rf $CERT && mkdir -p $CERT
    printf 'basicConstraints=critical,CA:TRUE\nkeyUsage=critical,keyCertSign,cRLSign\nsubjectKeyIdentifier=hash\n' > $CERT/root.ext
    printf 'basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=codeSigning\nsubjectKeyIdentifier=hash\nauthorityKeyIdentifier=keyid\n' > $CERT/leaf.ext
    openssl req -new -newkey rsa:2048 -nodes -keyout $CERT/root.key -subj "/CN=Show2Cam Test Root CA" -out $CERT/root.csr 2>/dev/null
    openssl x509 -req -in $CERT/root.csr -signkey $CERT/root.key -sha256 -days 3650 -extfile $CERT/root.ext -out $CERT/root.pem 2>/dev/null
    openssl req -new -newkey rsa:2048 -nodes -keyout $CERT/key.pem -subj "/CN=Show2Cam Test Signing" -out $CERT/leaf.csr 2>/dev/null
    openssl x509 -req -in $CERT/leaf.csr -CA $CERT/root.pem -CAkey $CERT/root.key -CAcreateserial -sha256 \
        -days 3650 -extfile $CERT/leaf.ext -out $CERT/cert.pem 2>/dev/null
    rm -f $CERT/*.csr $CERT/*.srl
fi
cat $CERT/cert.pem $CERT/root.pem > $CERT/chain.pem
openssl x509 -in $CERT/root.pem -outform DER -out "$OUT/Show2Cam.cer"
openssl x509 -in $CERT/cert.pem -outform DER -out "$OUT/Show2Cam-Publisher.cer"

rm -f "$OUT/Show2Cam.sys"
osslsigncode sign -certs $CERT/chain.pem -key $CERT/key.pem -h sha256 -n "Show2Cam" \
    -in "$OBJ/Show2Cam.sys" -out "$OUT/Show2Cam.sys" >/dev/null
python3 pefix.py "$OUT/Show2Cam.sys" --checksum-only
rm -rf "$OBJ"
echo "built and test-signed ($ARCH): driver/mingw/$OUT/Show2Cam.sys"
