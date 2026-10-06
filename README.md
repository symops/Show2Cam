# Show2Cam — virtual webcams for Windows 10/11

Show2Cam adds up to 10 virtual cameras ("Show2Cam Camera 1" … "Show2Cam Camera 10") for every program that uses a
webcam. What each camera shows is chosen in the control panel. Every camera offers its own size plus the usual webcam
sizes (1920×1080 … 320×240) at 1–60 fps; the device is in the Image class, as classic webcams and e2eSoft VCam / ManyCam
(older programs, e.g. DLP agents, look for webcams there only). A sister project of
[Speak2Mic](https://github.com/symops/Speak2Mic) (virtual audio cable).

## Control panel (Show2Cam.exe)

* **Cameras** — the list of the cameras with their source, format and who uses each one right now (the
  program's name, or "in use" when Windows does not tell); the number of cameras (1–10; one Windows device per camera, as one-camera drivers: devices are added / removed with administrator rights, the other cameras keep running).
* **The selected camera**
  * **Name** — the name programs show; renaming needs no administrator rights (the driver keeps it).
  * **Source**
    * *Text* (default "Camera 1", "Camera 2", …), drawn as large as fits;
    * *Images from a folder* — PNG, JPEG, BMP, GIF, TIFF, ICO, JPEG XR, and WebP / HEIF / AVIF when Windows has
      their codecs; a random picture every 5 seconds, never the same one twice in a row while there is another;
    * *Videos from a folder* — MP4, M4V, MOV, 3GP/3G2, F4V, WMV/ASF, AVI (DivX, XviD), MKV, WebM, MPEG-TS/M2TS/MTS,
      MPG/MPEG/VOB, DV (Windows' Media Foundation and its codecs; a file whose extension Windows does not know is
      recognised by its content), random files by the same
      rule. Their sound plays in step with the picture on **Speak2Mic Speaker** (found by its adapter, so a renamed
      one is found too), on another playback device, or not at all;
    * *Generator* — an animated picture made up on the fly: a slowly changing gradient with moving, turning shapes;
    * *MJPEG stream* over HTTP/HTTPS (e.g. `http://10.0.28.101:8001`, `http://user:password@host/…`). Without
      pictures the camera shows "No signal" (in the interface language) and reconnects every 3 seconds.
  * **Resolution and frame rate** — "as the source" (default) or chosen. They change while no program has the
    camera open; otherwise as soon as it is closed.
  * **Apply** puts the camera's settings (source, text / folder / address, sound, size, frame rate) into
    effect; switching to another camera or closing the panel with unsaved changes asks whether to keep them.
  * **Check** — a live preview window of that camera (several cameras' windows may be open at once); **Play / Pause**.
* Settings export / import (an .ini file, as Speak2Mic): the number of cameras and every camera's name, source,
  folders, address, sound, size, frame rate and pause.
* Event list, start with Windows in the tray (on by default: the cameras show their sources while the panel runs;
  without it they show the driver's test pattern), 17 languages.

The default folders are `images` and `mp4` next to the program (`C:\Program Files\Show2Cam`).

## Installation

Windows must run in test signing mode with Secure Boot off (VMware: VM → Settings → Options → Advanced → clear
"Enable UEFI Secure Boot"). Run `Show2Cam-Setup.exe`; if test signing is off, click "Enable test mode", restart and
run it again, then "Install". The cameras appear as "Show2Cam Camera 1" … in every program that uses a webcam.

`s2cinstall.exe` (administrator command prompt): `status` (cameras, frames, driver log), `cameras N`,
`size N WIDTH HEIGHT [FPS]`, `color N RRGGBB`, `bmp N FILE.bmp`, `pattern N`, `install`, `remove`.

`s2cctl.exe` — what the control panel sets, from the command line (no administrator rights except `count`):
`status`, `count N`, `name CAM "NAME"`, `source CAM text ["TEXT"] | images [FOLDER] | video [FOLDER] | stream URL |
generator`, `audio CAM speak2mic|off|DEVICE`, `format CAM source|WxH [FPS|source]`, `pause CAM on|off`, `reset CAM`,
`export FILE.ini`, `import FILE.ini`, `picture CAM FILE|pattern` (CAM: 1..10 or `all`). A running panel takes the
changes at once. Exit codes: 0 ok, 1 failed, 2 bad arguments, 3 administrator rights needed, 4 Show2Cam cannot work
here. Log: `ctl.log`.

`s2cautotest.exe [minutes] [--seed N]` (administrator; default 10 minutes): randomized end-to-end test as programs
see the cameras (Media Foundation) — a picture of four coloured quarters read back (colours, orientation, size, in use
while open), the test pattern not "empty", random formats (set, offered first, invalid ones refused), the usual webcam
sizes and 640×480 at 3 fps, random names (camera, device FriendlyName), random camera counts (one device per camera),
the panel's sources (text, generator), open/close stress and `s2cctl` commands; and with media it generates in
`%TEMP%\s2cautotest-<pid>` (removed at the end): random texts, pictures in every format Windows encodes (PNG, JPEG,
BMP, GIF, TIFF, JPEG XR; plus a broken one), video clips (MP4 H.264 + AAC stereo / 5.1 / silent, 3GP, WMV + WMA, the MP4
as .mov / .m4v / .divx, a broken file) whose 1 kHz tone is measured on Speak2Mic Microphone on the second pass, and a
local MJPEG server (colour change, outage and reconnect, Basic authentication). The formats Windows does not write are
built into the autotest as samples, made anew on every build by `tools/make_autotest_media.py` (ffmpeg, libheif, PIL):
WebP / HEIC / AVIF / ICO pictures, MKV (H.264 + AC-3 / Vorbis), WebM (VP9 + Opus, VP8 + Vorbis), AVI (Xvid + MP3, MJPEG +
PCM), MPEG-TS, M2TS, MPEG-1/2 PS, VOB, DV, MP4 with MP3 sound / HEVC / AV1, MOV with ALAC, WMV8 + WMA2 and a portrait
clip; a sample that needs a decoder from a Store extension Windows does not have is a WARN, not a failure. It closes the control panel, restores
everything at the end (also after Ctrl+C) and starts the panel again. PASS / FAIL / WARN in `autotest.log`; exit code
1 if anything failed.

`s2ccamdiag.exe` (and `s2ccamdiag32.exe` for 32-bit programs): how programs see every camera (Show2Cam and others) —
the Windows registration, camera privacy, DirectShow (formats, choosing 640×480 / 1280×720 / 1920×1080, a capture graph
running for 3 s), Media Foundation (formats, frames read) and Video for Windows; `--no-run` lists only. Log:
`camdiag-x64.log` / `camdiag-x86.log`.

Logs: `C:\ProgramData\Show2Cam\logs\` — `setup.log` (installer), `install.log` (s2cinstall), `ctl.log` (s2cctl), `autotest.log` (s2cautotest), `panel.log`
(control panel), `events.log` (the panel's event list), `driver.log` (written by the driver itself).

## Building (Linux)

```
./package.sh
```
needs clang, MinGW-w64 for x86_64 and i686, a host C compiler and make, osslsigncode, openssl, gcab and Python 3
(Pillow and NumPy only for `app/make_icon.py`). Output: `dist/Show2Cam-Setup.exe` (the whole package in one file) and
the same unpacked in `dist/Show2Cam/`.

## Author

Symo — symops@gmail.com
