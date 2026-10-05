#!/bin/sh
# Builds everything on Linux (clang + MinGW-w64, no WDK) and assembles the package:
#   dist/Show2Cam/x64/  dist/Show2Cam/x86/   driver (.sys, .inf, signed .cat), test certificates, programs
set -e
cd "$(dirname "$0")"
python3 gen.py
ARCH=x64 driver/mingw/build.sh
ARCH=x86 driver/mingw/build.sh
ARCH=x64 app/build.sh
ARCH=x86 app/build.sh
make -s -C tools/generate-cat-file

D=dist/Show2Cam
rm -rf dist
for ARCH in x64 x86; do
    A=$D/$ARCH
    mkdir -p $A
    if [ $ARCH = x64 ]; then DRVOUT=driver/mingw/out; APP=app; MODEL=NTamd64; OSATTR=_v100_X64
    else DRVOUT=driver/mingw/out-x86; APP=app/x86; MODEL=NTx86; OSATTR=_v100; fi
    cp $DRVOUT/Show2Cam.sys $DRVOUT/Show2Cam.cer $DRVOUT/Show2Cam-Publisher.cer $A/
    cp $APP/s2cinstall.exe $A/

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
(cd dist && rm -f Show2Cam.zip && zip -qr Show2Cam.zip Show2Cam)
ls -la dist/Show2Cam.zip
