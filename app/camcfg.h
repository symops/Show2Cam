// Settings of the cameras' sources, per user (HKCU\Software\Show2Cam\Camera<N>), and the settings file (export /
// import: UTF-16 INI). Shared by the panel, s2cctl and the autotest.
//   [Show2Cam]  Version, CameraCount
//   [Camera<N>] Name, Source, Text, ImageFolder, VideoFolder, Url, AudioMode, AudioDevice, Width, Height, Fps, Paused,
//               Enabled
#pragma once

#include "sources.h"

#define S2C_USER_KEY    L"Software\\Show2Cam"
#define S2C_PANEL_CLASS L"S2cPanel"
// Registered window message (RegisterWindowMessageW) the running panel gets after another program (s2cctl, the
// autotest) changed the settings in the registry: it reads them again. wParam: camera index, or -1 for all.
#define S2C_SETTINGS_CHANGED_MSG L"Show2Cam.SettingsChanged"

void CamConfigDefault(int index, CamConfig* c);
void CamConfigLoad(int index, CamConfig* c);
void CamConfigSave(int index, const CamConfig& c);
void CamConfigDelete(int index);                // back to the defaults (a camera deleted)
// Cameras reordered (deleted, or one given another number): new camera i (0..n-1) is old camera src[i]. Its per-user
// settings follow; cameras n..count-1 go back to the defaults.
void CamConfigPermute(const int* src, int n, int count);
// The names of cameras 0..count-1 now (Windows' list, else the driver settings); then, after a reordering, the cameras
// Windows offers take the names of the ones they now are (a camera switched off has its name in the driver settings,
// which SetupMoveCamera / SetupDeleteCamera reorder).
void CamNamesNow(int count, wchar_t (*names)[S2C_NAME_CHARS]);
void CamNamesAfterPermute(const int* src, int n, wchar_t (*names)[S2C_NAME_CHARS]);
// The reorderings themselves: the permutation of a deletion / a move (returns n).
int CamDeletePermutation(int index, int count, int* src);
int CamMovePermutation(int from, int to, int count, int* src);
// The camera's name kept by the driver (also while it is switched off), else the default; S2C_NAME_CHARS.
void CamSavedName(int index, wchar_t* out);
void CamConfigNotifyPanel(int index);           // -1: all cameras
bool CamPanelRunning();

// Settings file. Writing: create (UTF-16 BOM), the header, every camera, flush (false: not written).
bool CamIniCreate(const wchar_t* path);
void CamIniWriteHeader(const wchar_t* path, int cameraCount);
void CamIniWriteCamera(const wchar_t* path, int index, const wchar_t* name, const CamConfig& c);
bool CamIniFlush(const wchar_t* path);
// Reading. CamIniCheck: false if the file is not Show2Cam settings; *count = CameraCount 0..10 (-1: not in the file).
bool CamIniCheck(const wchar_t* path, int* count);
// Camera `index` (0-based) from the file over *c (values missing or invalid in the file keep *c). false: the file has
// no such camera. name: S2C_NAME_CHARS; *hasName = the file has a name ("" = the default name).
bool CamIniReadCamera(const wchar_t* path, int index, CamConfig* c, wchar_t* name, bool* hasName);
