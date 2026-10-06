# Show2Cam — virtual webcams for Windows 10/11

Show2Cam adds up to 10 virtual cameras ("Show2Cam Camera 1" … "Show2Cam Camera 10") for every program that uses a
webcam. What each camera shows is chosen in the control panel. Every camera offers its own size plus the usual webcam
sizes (1920×1080 … 320×240) at 1–60 fps; the device is in the Image class, as classic webcams and e2eSoft VCam / ManyCam
(older programs, e.g. DLP agents, look for webcams there only). A sister project of
[Speak2Mic](https://github.com/symops/Speak2Mic) (virtual audio cable).

## Control panel (Show2Cam.exe)

* **Cameras** — the list of the cameras with their source, format and who uses each one right now ("self-view", the
  program's name, or "in use" when Windows does not tell); the number of cameras (1–10; one Windows device per camera, as one-camera drivers: devices are added / removed with administrator rights, the other cameras keep running).
* **The selected camera**
  * **Name** — the name programs show; renaming needs no administrator rights (the driver keeps it).
  * **Source**
    * *Text* (default "Camera 1", "Camera 2", …), drawn as large as fits;
    * *Images from a folder* — PNG, JPEG, BMP, GIF, TIFF, ICO, JPEG XR, and WebP / HEIF / AVIF when Windows has
      their codecs; a random picture every 5 seconds, never the same one twice in a row while there is another;
    * *Videos from a folder* — MP4, MOV, M4V, WMV, AVI, MKV, WebM, … (Media Foundation), random files by the same
      rule. Their sound plays in step with the picture on **Speak2Mic Speaker** (found by its adapter, so a renamed
      one is found too), on another playback device, or not at all;
    * *Generator* — an animated picture made up on the fly: a slowly changing gradient with moving, turning shapes;
    * *MJPEG stream* over HTTP/HTTPS (e.g. `http://10.0.28.101:8001`, `http://user:password@host/…`). Without
      pictures the camera shows "No signal" (in the interface language) and reconnects every 3 seconds.
  * **Resolution and frame rate** — "as the source" (default) or chosen. They change while no program has the
    camera open; otherwise as soon as it is closed.
  * **Apply** puts the camera's settings (source, text / folder / address, sound, size, frame rate, self-view) into
    effect; switching to another camera or closing the panel with unsaved changes asks whether to keep them.
  * **Check** — a live preview window of that camera (several cameras' windows may be open at once); **Play / Pause**.
  * **Self-view** — the panel itself uses the camera like any webcam program (Media Foundation, through the Windows
    camera stack): for Windows the camera is really in use (the "camera in use" indicator, Settings → Privacy →
    Camera), as if someone were watching it. The size / frame rate can still change: the panel closes its own use
    for that moment.
* Settings export / import (an .ini file, as Speak2Mic): the number of cameras and every camera's name, source,
  folders, address, sound, size, frame rate, pause and self-view.
* Event list, start with Windows in the tray (on by default: the cameras show their sources while the panel runs;
  without it they show the driver's test pattern), 17 languages.

The default folders are `images` and `mp4` next to the program (`C:\Program Files\Show2Cam`).

## Installation

Windows must run in test signing mode with Secure Boot off (VMware: VM → Settings → Options → Advanced → clear
"Enable UEFI Secure Boot"). Run `Show2Cam-Setup.exe`; if test signing is off, click "Enable test mode", restart and
run it again, then "Install". The cameras appear as "Show2Cam Camera 1" … in every program that uses a webcam.

`s2cinstall.exe` (administrator command prompt): `status` (cameras, frames, driver log), `cameras N`,
`size N WIDTH HEIGHT [FPS]`, `color N RRGGBB`, `bmp N FILE.bmp`, `pattern N`, `install`, `remove`.

`s2ccamdiag.exe` (and `s2ccamdiag32.exe` for 32-bit programs): how programs see every camera (Show2Cam and others) —
the Windows registration, camera privacy, DirectShow (formats, choosing 640×480 / 1280×720 / 1920×1080, a capture graph
running for 3 s), Media Foundation (formats, frames read) and Video for Windows; `--no-run` lists only. Log:
`camdiag-x64.log` / `camdiag-x86.log`.

Logs: `C:\ProgramData\Show2Cam\logs\` — `setup.log` (installer), `install.log` (s2cinstall), `panel.log`
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
