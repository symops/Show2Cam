#!/usr/bin/env python3
"""Makes the sample files of the formats Windows cannot encode itself, for s2cautotest. Run by app/build.sh on every
build (a new set each time: the pictures' colours are picked at random, the manifest says which), not kept in git.
They are built into the autotest as resources (autotest_media.rc, manifest.txt) and written out to the test's temporary
folder at its start, then removed with it.

Show2Cam: pictures (WebP, HEIC, AVIF, ICO) in one known colour each; video clips in the containers / codecs Windows
plays but does not write (MKV, WebM, AVI, MPEG-TS, M2TS, MPEG-PS / VOB, DV, MP4 with HEVC / AV1 / MP3 sound, MOV with
ALAC, ASF with WMV8), all with the autotest's picture (colour fields changing every 0.5 s, see atmedia.h) and its
1 kHz tone.

  python3 tools/make_autotest_media.py      (needs ffmpeg with libx264/x265/vpx/aom/mp3lame/vorbis/opus/wmv2, heif-enc, PIL)
The manifest lines: kind|file|parameters (image: colour RRGGBB; video: minimum colour fields seen, sound 0/1, needs an
optional Windows decoder 0/1; music: -).
"""
import math, os, random, struct, subprocess, sys, tempfile
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
S2C = os.path.join(HERE, '..', 'app', 'autotest-media')

# atmedia.cpp: kTestColors, clip colour fields kTestColors[0..3] every 0.5 s
COLORS = [0xCC3333, 0x33CC33, 0x3333CC, 0xCCCC33, 0xCC33CC, 0x33CCCC, 0xFF9900, 0x996633]
W, H = 320, 240


def rgb(c):
    return ((c >> 16) & 255, (c >> 8) & 255, c & 255)


def run(args):
    r = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    if r.returncode:
        sys.stderr.write(r.stderr.decode(errors='replace')[-2000:])
        raise SystemExit('failed: ' + ' '.join(args))


def picture(color, w=W, h=H):
    """atmedia.cpp DrawPicture: the colour, a checkered border, a white band near the top."""
    img = Image.new('RGB', (w, h), rgb(color))
    px = img.load()
    for y in range(h):
        for x in range(w):
            border = x < w // 10 or x >= w - w // 10 or y < h // 10 or y >= h - h // 10
            if border and ((x // 16 + y // 16) & 1):
                px[x, y] = (0, 0, 0)
            if h // 10 <= y < h // 10 + 12:
                px[x, y] = (255, 255, 255)
    return img


def clip_frames(path, w, h, fps, seconds):
    """Raw RGB frames of the autotest's clip: colour field kTestColors[(t / 0.5) % 4], a white square moving."""
    n = int(round(seconds * fps))
    with open(path, 'wb') as f:
        for i in range(n):
            t = i / fps
            img = Image.new('RGB', (w, h), rgb(COLORS[int(t / 0.5) % 4]))
            sq = max(16, h // 8)
            sx = (i * 8) % (w - sq)
            img.paste((255, 255, 255), (sx, h // 15, sx + sq, h // 15 + sq))
            f.write(img.tobytes())
    return n


def boxes(data, start=0, end=None):
    """(type, payload start, end) of the ISO-BMFF boxes in data[start:end]."""
    end = len(data) if end is None else end
    pos = start
    while pos + 8 <= end:
        size, typ = struct.unpack('>I4s', data[pos:pos + 8])
        hdr = 8
        if size == 1:
            size = struct.unpack('>Q', data[pos + 8:pos + 16])[0]
            hdr = 16
        elif size == 0:
            size = end - pos
        yield typ.decode('latin-1'), pos + hdr, pos + size
        pos += size


def find(data, path, start=0, end=None):
    for typ, a, b in boxes(data, start, end):
        if typ == path[0]:
            if len(path) == 1:
                return a, b
            skip = {'stsd': 8, 'hvc1': 78, 'hev1': 78}.get(typ, 0)
            r = find(data, path[1:], a + skip, b)
            if r:
                return r
    return None


def box(typ, payload, full=None):
    if full is not None:
        payload = struct.pack('>I', full) + payload        # version 0 + flags
    return struct.pack('>I4s', 8 + len(payload), typ.encode()) + payload


def write_heic(img, path):
    """HEIC without an HEIF encoder: one HEVC intra frame (x265 via ffmpeg, in an MP4), its hvcC and coded data put
    into an HEIF container (ftyp, meta: hdlr pict / pitm / iinf hvc1 / iloc / iprp hvcC + ispe, mdat)."""
    tmp = os.path.join(tempfile.gettempdir(), 'atm_heic')
    img.save(tmp + '.png')
    run(['ffmpeg', '-y', '-loglevel', 'error', '-i', tmp + '.png', '-frames:v', '1', '-c:v', 'libx265', '-crf', '24', '-pix_fmt',
         'yuv420p', '-tag:v', 'hvc1', '-x265-params', 'log-level=error:keyint=1', tmp + '.mp4'])
    mp4 = open(tmp + '.mp4', 'rb').read()
    stbl = ['moov', 'trak', 'mdia', 'minf', 'stbl']
    a, b = find(mp4, stbl + ['stsd', 'hvc1', 'hvcC'])
    hvcc = mp4[a:b]
    a2, b2 = find(mp4, stbl + ['stsz'])
    size = struct.unpack('>I', mp4[a2 + 4:a2 + 8])[0] or struct.unpack('>I', mp4[a2 + 12:a2 + 16])[0]
    a3, b3 = find(mp4, stbl + ['stco'])
    offset = struct.unpack('>I', mp4[a3 + 8:a3 + 12])[0]
    coded = mp4[offset:offset + size]
    w, h = img.size
    ftyp = box('ftyp', b'heic' + struct.pack('>I', 0) + b'mif1heic')
    hdlr = box('hdlr', struct.pack('>I', 0) + b'pict' + b'\0' * 12 + b'\0', 0)
    pitm = box('pitm', struct.pack('>H', 1), 0)
    infe = box('infe', struct.pack('>HH', 1, 0) + b'hvc1' + b'\0', 0x02000000)
    iinf = box('iinf', struct.pack('>H', 1) + infe, 0)
    ipco = box('ipco', box('hvcC', hvcc) + box('ispe', struct.pack('>II', w, h), 0))
    ipma = box('ipma', struct.pack('>IHB', 1, 1, 2) + bytes([0x81, 0x02]), 0)
    iprp = box('iprp', ipco + ipma)
    def build(mdat_offset):
        iloc = box('iloc', bytes([0x44, 0x00]) + struct.pack('>H', 1) + struct.pack('>HHHII', 1, 0, 1, mdat_offset, len(coded)), 0)
        return ftyp + box('meta', hdlr + pitm + iloc + iinf + iprp, 0)
    head = build(0)
    head = build(len(head) + 8)
    with open(path, 'wb') as f:
        f.write(head + box('mdat', coded))


def make_s2c(out):
    os.makedirs(out, exist_ok=True)
    for f in os.listdir(out):
        os.remove(os.path.join(out, f))
    manifest = []
    # pictures
    picks = random.sample(range(8), 4)                     # a new colour for each picture on every build
    pics = list(zip(['webp', 'heic', 'avif', 'ico'], picks))
    for ext, ci in pics:
        name = 'sample.' + ext
        p = os.path.join(out, name)
        img = picture(COLORS[ci], 256 if ext == 'ico' else W, 256 if ext == 'ico' else H)
        if ext == 'webp':
            img.save(p, 'WEBP', quality=90)
        elif ext == 'ico':
            img.save(p, 'ICO', sizes=[(256, 256)])
        elif ext == 'heic':
            write_heic(img, p)
        else:
            tmp = os.path.join(tempfile.gettempdir(), 'atm_pic.png')
            img.save(tmp)
            run(['heif-enc', '-q', '90', '-A', '-o', p, tmp])
        manifest.append('image|%s|%06X' % (name, COLORS[ci]))

    # video clips: (file, ffmpeg video args, audio args or None, seconds, size, needs an optional decoder)
    v264 = ['-c:v', 'libx264', '-preset', 'veryslow', '-crf', '30', '-pix_fmt', 'yuv420p']
    aac = ['-c:a', 'aac', '-b:a', '64k']
    clips = [
        ('mkv-h264-ac3.mkv', v264, ['-c:a', 'ac3', '-b:a', '96k'], 3, (W, H), 0),
        ('mkv-h264-vorbis.mkv', v264, ['-c:a', 'libvorbis', '-q:a', '1'], 3, (W, H), 1),        # Vorbis: Web Media Extensions
        ('webm-vp9-opus.webm', ['-c:v', 'libvpx-vp9', '-b:v', '150k', '-deadline', 'good'], ['-c:a', 'libopus', '-b:a', '48k'], 3, (W, H), 1),
        ('webm-vp8-vorbis.webm', ['-c:v', 'libvpx', '-b:v', '200k'], ['-c:a', 'libvorbis', '-q:a', '1'], 3, (W, H), 1),
        ('avi-xvid-mp3.avi', ['-c:v', 'mpeg4', '-vtag', 'XVID', '-q:v', '8'], ['-c:a', 'libmp3lame', '-b:a', '64k'], 3, (W, H), 0),
        ('avi-mjpeg-pcm.avi', ['-c:v', 'mjpeg', '-q:v', '8', '-pix_fmt', 'yuvj420p'], ['-c:a', 'pcm_s16le', '-ar', '22050'], 3, (W, H), 0),
        ('ts-h264-aac.ts', v264, aac, 3, (W, H), 0),
        ('m2ts-h264-ac3.m2ts', v264 + ['-mpegts_m2ts_mode', '1'], ['-c:a', 'ac3', '-b:a', '96k'], 3, (W, H), 0),
        ('mpg-mpeg2-mp2.mpg', ['-c:v', 'mpeg2video', '-q:v', '6', '-f', 'mpeg'], ['-c:a', 'mp2', '-b:a', '96k'], 3, (W, H), 0),
        ('mpg-mpeg1-mp2.mpg', ['-c:v', 'mpeg1video', '-q:v', '6', '-f', 'mpeg'], ['-c:a', 'mp2', '-b:a', '96k'], 3, (W, H), 0),
        ('vob-mpeg2-ac3.vob', ['-c:v', 'mpeg2video', '-q:v', '6', '-f', 'vob'], ['-c:a', 'ac3', '-b:a', '96k'], 3, (W, H), 0),
        ('dv-pal.dv', ['-c:v', 'dvvideo', '-pix_fmt', 'yuv420p', '-f', 'dv'], ['-c:a', 'pcm_s16le', '-ar', '48000', '-ac', '2'], 0.6, (720, 576), 0),
        ('mp4-h264-mp3.mp4', v264, ['-c:a', 'libmp3lame', '-b:a', '64k'], 3, (W, H), 0),
        ('mp4-hevc-aac.mp4', ['-c:v', 'libx265', '-crf', '32', '-tag:v', 'hvc1', '-x265-params', 'log-level=error'], aac, 3, (W, H), 1),
        ('mp4-av1-aac.mp4', ['-c:v', 'libaom-av1', '-crf', '45', '-cpu-used', '8'], aac, 3, (W, H), 1),
        ('mov-h264-alac.mov', v264, ['-c:a', 'alac'], 3, (W, H), 0),
        ('wmv-wmv8-wma2.wmv', ['-c:v', 'wmv2', '-q:v', '6'], ['-c:a', 'wmav2', '-b:a', '64k'], 3, (W, H), 0),
        ('mp4-h264-portrait.mp4', v264, aac, 3, (H, W), 0),          # another aspect ratio (portrait phone video)
    ]
    for name, vargs, aargs, seconds, (w, h), optional in clips:
        fps = 25 if name.startswith(('dv', 'mpg', 'vob')) else 15
        raw = os.path.join(tempfile.gettempdir(), 'atm_frames.rgb')
        clip_frames(raw, w, h, fps, seconds)
        args = ['ffmpeg', '-y', '-loglevel', 'error', '-f', 'rawvideo', '-pix_fmt', 'rgb24', '-s', '%dx%d' % (w, h), '-r', str(fps),
                '-i', raw]
        if aargs:
            args += ['-f', 'lavfi', '-i', 'sine=frequency=1000:sample_rate=48000:duration=%g,volume=0.25' % seconds]
        fmt = [a for a in vargs if a in ('-f',)]
        args += [a for a in vargs]
        if aargs:
            args += aargs + ['-ac', '2' if 'ac3' not in ' '.join(aargs) else '2']
        args += ['-shortest', os.path.join(out, name)]
        run(args)
        colors = 1 if seconds < 1 else 3
        manifest.append('video|%s|%d|%d|%d' % (name, colors, 1 if aargs else 0, optional))
    with open(os.path.join(out, 'manifest.txt'), 'w') as f:
        f.write('\n'.join(manifest) + '\n')
    return manifest


def write_rc(folder, manifest, first_id):
    """RCDATA resources: id first_id = manifest.txt, then the files in manifest order."""
    lines = ['// Generated by tools/make_autotest_media.py: the autotest sample files (RCDATA).',
             '%d RCDATA "autotest-media/manifest.txt"' % first_id]
    for i, m in enumerate(manifest):
        lines.append('%d RCDATA "autotest-media/%s"' % (first_id + 1 + i, m.split('|')[1]))
    with open(os.path.join(folder, '..', 'autotest_media.rc'), 'w') as f:
        f.write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    m = make_s2c(S2C)
    write_rc(S2C, m, 500)
    total = sum(os.path.getsize(os.path.join(S2C, f)) for f in os.listdir(S2C))
    print('autotest samples: %d files, %d KB in %s' % (len(os.listdir(S2C)), total // 1024, os.path.normpath(S2C)))
