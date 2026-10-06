#!/bin/sh
# Builds everything on Linux (clang + MinGW-w64, no WDK) and assembles the package:
#   dist/Show2Cam-Setup.exe     the release: ONE file - the launcher with the whole package below appended
#                               (a cabinet, MSZIP; extracted to a temporary folder when it runs)
#   dist/Show2Cam/              the same package unpacked (for development / testing)
#     Show2Cam-Setup.exe        x86 launcher: starts x64\ or x86\Show2Cam-Setup.exe (whichever Windows this is)
#     uninstall.cmd             removal without a window
#     images\  mp4\              media for the cameras (installed to Program Files\Show2Cam)
#     x64\  x86\                 the programs, the driver (.sys, .inf, .cat) and the test certificates of each
set -e
cd "$(dirname "$0")"
python3 gen.py
python3 app/lang/gen_lang.py
ARCH=x64 driver/mingw/build.sh
ARCH=x86 driver/mingw/build.sh
ARCH=x64 app/build.sh
ARCH=x86 app/build.sh
make -s -C tools/generate-cat-file

D=dist/Show2Cam
rm -rf dist
mkdir -p $D/images $D/mp4
cp app/x86/s2clauncher.exe $D/Show2Cam-Setup.exe
cp scripts/uninstall.cmd $D/
for f in media/images/* media/mp4/*; do [ -f "$f" ] && cp "$f" $D/$(basename $(dirname "$f"))/; done
for ARCH in x64 x86; do
    A=$D/$ARCH
    mkdir -p $A
    if [ $ARCH = x64 ]; then DRVOUT=driver/mingw/out; APP=app; MODEL=NTamd64; OSATTR=_v100_X64
    else DRVOUT=driver/mingw/out-x86; APP=app/x86; MODEL=NTx86; OSATTR=_v100; fi
    cp $DRVOUT/Show2Cam.sys $DRVOUT/Show2Cam.cer $DRVOUT/Show2Cam-Publisher.cer $A/
    cp $APP/Show2Cam.exe $APP/Show2Cam-Setup.exe $APP/s2cinstall.exe $APP/s2ccamdiag.exe $A/
    # 32-bit programs use the 32-bit DirectShow / Media Foundation: the x86 diagnostics next to the x64 one
    [ $ARCH = x64 ] && cp app/x86/s2ccamdiag.exe $A/s2ccamdiag32.exe
    cp scripts/uninstall.cmd $A/

    # INF of this architecture: gen.py writes NTamd64 and NTarm64 models; keep NTamd64 (x64) or turn it into NTx86.
    python3 - "$A/Show2Cam.inf" "$MODEL" <<'PY'
import sys
src = open('driver/Show2Cam.inf', encoding='utf-8').read().replace('\r\n', '\n')
model = sys.argv[2]
out, skip = [], False
for line in src.split('\n'):
    if line.startswith('%MfgName%=Show2Cam,'):
        line = line.split(',NTarm64')[0].replace('NTamd64', model)    # keeps the TargetOSVersion decoration
    if line.startswith('['):
        skip = line.strip().startswith('[Show2Cam.NTarm64')
        if line.startswith('[Show2Cam.NTamd64'):
            line = line.replace('NTamd64', model)
    if skip:
        continue
    out.append(line)
open(sys.argv[1], 'w', encoding='utf-8', newline='\r\n').write('\n'.join(out))
PY

    # Catalog: hashes of the INF and SYS (Windows refuses a package without one, 0xE000022F), test-signed.
    tools/generate-cat-file/gencat.sh -o $A/Show2Cam.cat.unsigned -h 'root\show2cam' \
        -O $OSATTR -A 2:10.0 -T "$(date -u +%y%m%d%H%M%SZ)" $A/Show2Cam.inf $A/Show2Cam.sys
    osslsigncode sign -certs driver/mingw/testcert/chain.pem -key driver/mingw/testcert/key.pem -h sha256 \
        -n "Show2Cam" -in $A/Show2Cam.cat.unsigned -out $A/Show2Cam.cat >/dev/null
    rm -f $A/Show2Cam.cat.unsigned
    osslsigncode verify -CAfile driver/mingw/testcert/root.pem -in $A/Show2Cam.cat 2>&1 | grep -q "Signature verification: ok" \
        || { echo "catalog signature check failed ($ARCH)"; exit 1; }
    echo "catalog: $A/Show2Cam.cat (signed)"
done
# Windows cmd.exe reads .cmd files in the OEM code page; convert the Russian text to CP866.
for f in $D/uninstall.cmd $D/x64/uninstall.cmd $D/x86/uninstall.cmd; do
    iconv -f UTF-8 -t CP866 "$f" | sed 's/$/\r/' > "$f.tmp" && mv "$f.tmp" "$f"
done

# One file: the launcher + the package as a cabinet (MSZIP; Windows extracts it with SetupIterateCabinet) + a trailer
# "S2CPAYLD" + the cabinet's size (little endian, 8 bytes). See app/s2clauncher.cpp.
(cd $D && find uninstall.cmd images mp4 x64 x86 -type f | sort | xargs gcab -c -z ../payload.cab)
python3 - <<'PY'
import struct
launcher = open('app/x86/s2clauncher.exe', 'rb').read()
cab = open('dist/payload.cab', 'rb').read()
open('dist/Show2Cam-Setup.exe', 'wb').write(launcher + cab + b'S2CPAYLD' + struct.pack('<Q', len(cab)))
PY
rm -f dist/payload.cab
ls -la dist/Show2Cam-Setup.exe
