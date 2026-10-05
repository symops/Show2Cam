# Show2Cam — virtual webcams for Windows 10/11

Show2Cam adds up to 10 virtual cameras ("Show2Cam Camera 1" … "Show2Cam Camera 10") for every program that uses a
webcam. What each camera shows is chosen in the control panel. A sister project of
[Speak2Mic](https://github.com/symops/Speak2Mic) (virtual audio cable).

## Control panel (Show2Cam.exe)

* **Cameras** — the list of the cameras with their source, format and an "in use" indicator (a program has the camera
  open); the number of cameras (1–10; a device restart, asks for administrator rights).
* **The selected camera**
  * **Name** — the name programs show; renaming needs no administrator rights (the driver keeps it).
  * **Source**
    * *Text* (default "Camera 1", "Camera 2", …), drawn as large as fits;
    * *Images from a folder* — PNG, JPEG, BMP, GIF, TIFF, ICO, JPEG XR, and WebP / HEIF / AVIF when Windows has
      their codecs; a random picture every 5 seconds, never the same one twice in a row while there is another;
    * *Videos from a folder* — MP4, MOV, M4V, WMV, AVI, MKV, WebM, … (Media Foundation), random files by the same
      rule. Their sound plays in step with the picture on **Speak2Mic Speaker** (found by its adapter, so a renamed
      one is found too), on another playback device, or not at all;
    * *MJPEG stream* over HTTP/HTTPS (e.g. `http://10.0.28.101:8001`, `http://user:password@host/…`). Without
      pictures the camera shows "No signal" (in the interface language) and reconnects every 3 seconds.
  * **Resolution and frame rate** — "as the source" (default) or chosen. They change while no program has the
    camera open; otherwise as soon as it is closed.
  * **Check** — a live preview window of that camera; **Play / Pause**.
* Event list, start with Windows in the tray (on by default: the cameras show their sources while the panel runs;
  without it they show the driver's test pattern), 17 languages.

The default folders are `images` and `mp4` next to the program (`C:\Program Files\Show2Cam`).

## Installation

Windows must run in test signing mode with Secure Boot off (VMware: VM → Settings → Options → Advanced → clear
"Enable UEFI Secure Boot"). Run `Show2Cam-Setup.exe`; if test signing is off, click "Enable test mode", restart and
run it again, then "Install". The cameras appear as "Show2Cam Camera 1" … in every program that uses a webcam.

`s2cinstall.exe` (administrator command prompt): `status` (cameras, frames, driver log), `cameras N`,
`size N WIDTH HEIGHT [FPS]`, `color N RRGGBB`, `bmp N FILE.bmp`, `pattern N`, `install`, `remove`.

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
