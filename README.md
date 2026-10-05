# Show2Cam — virtual webcams for Windows 10/11

Work in progress. Show2Cam adds up to 10 virtual cameras ("Show2Cam Camera 1" … "Show2Cam Camera 10") whose picture
comes from text, PNG files, MP4 files or an MJPEG stream. A sister project of
[Speak2Mic](https://github.com/symops/Speak2Mic) (virtual audio cable).

Current state: the AVStream driver (N cameras, YUY2 / NV12 / RGB32, test pattern, pictures from a program through a
custom KS property) and the console tool `s2cinstall.exe`. The control panel, the installer and the sources follow.

## Building (Linux)

```
./package.sh
```
needs clang, MinGW-w64 for x86_64 and i686, a host C compiler and make, osslsigncode, openssl, zip and Python 3.
Output: `dist/Show2Cam/x64/` and `dist/Show2Cam/x86/` (test-signed driver, INF, catalog, certificates,
`s2cinstall.exe`). Windows must run in test signing mode with Secure Boot off.

## Author

Symo — symops@gmail.com
