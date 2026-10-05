# Show2Cam — virtual webcams for Windows 10/11

Work in progress. Show2Cam adds up to 10 virtual cameras ("Show2Cam Camera 1" … "Show2Cam Camera 10") whose picture
comes from text, PNG files, MP4 files or an MJPEG stream. A sister project of
[Speak2Mic](https://github.com/symops/Speak2Mic) (virtual audio cable).

Current state: the AVStream driver (N cameras, YUY2 / NV12 / RGB32, test pattern, pictures from a program through a
custom KS property), the installer (`Show2Cam-Setup.exe`, one file for x64 and x86, 17 languages, test signing mode
switch, Secure Boot check) and the console tool `s2cinstall.exe`. The control panel and the sources follow.

## Installation

Windows must run in test signing mode with Secure Boot off (VMware: VM → Settings → Options → Advanced → clear
"Enable UEFI Secure Boot"). Run `Show2Cam-Setup.exe`; if test signing is off, click "Enable test mode", restart and
run it again, then "Install". The cameras appear as "Show2Cam Camera 1" … in every program that uses a webcam.

`s2cinstall.exe` (administrator command prompt): `status` (cameras, frames, driver log), `cameras N`,
`size N WIDTH HEIGHT [FPS]`, `color N RRGGBB`, `bmp N FILE.bmp`, `pattern N`, `install`, `remove`.

## Building (Linux)

```
./package.sh
```
needs clang, MinGW-w64 for x86_64 and i686, a host C compiler and make, osslsigncode, openssl, gcab and Python 3
(Pillow and NumPy only for `app/make_icon.py`). Output: `dist/Show2Cam-Setup.exe` (the whole package in one file) and
the same unpacked in `dist/Show2Cam/`.

## Author

Symo — symops@gmail.com
