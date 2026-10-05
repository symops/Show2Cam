// Show2Cam device control shared by the panel, the installer, s2cctl and the autotest: driver settings in the
// registry and the device restart that makes the driver re-read them. Needs administrator rights.
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#define S2C_PARAMS_KEY  L"SYSTEM\\CurrentControlSet\\Services\\Show2Cam\\Parameters"
#define S2C_HARDWARE_ID L"ROOT\\Show2Cam"

// Disable + enable of every ROOT\Show2Cam device (like Device Manager): the driver re-creates its cameras with the
// new settings (camera count, resolutions). Programs using a camera lose it for that moment.
bool S2cRestartDevice(bool* found, bool* reboot);

// What the Show2Cam programs need before they can do anything: the driver is test-signed, so Windows loads it only
// in test signing mode, which needs Secure Boot off; and the driver (its device) must be installed. The panel,
// s2cctl and the autotest show the matching notice and refuse to run; the installers do not (they are what
// fixes it). With a Microsoft-signed driver only S2cNotReadyDriver would remain.
enum S2cNotReady { S2cReadyOk = 0, S2cNotReadySecureBoot, S2cNotReadyTestMode, S2cNotReadyDriver };
bool S2cSecureBootEnabled();        // UEFI Secure Boot on
bool S2cTestSigningEnabled();       // test signing mode active in the running system
bool S2cDriverInstalled();          // a ROOT\Show2Cam device exists
S2cNotReady S2cCheckReady();        // the first thing that is wrong, in that order
const wchar_t* S2cNotReadyTextEn(S2cNotReady reason);   // English, one line (console tools); ready.h has the translated one

// DWORD value under S2C_PARAMS_KEY; `def` if missing.
DWORD S2cGetParam(const wchar_t* name, DWORD def);
bool S2cSetParam(const wchar_t* name, DWORD value);
