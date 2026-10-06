// Settings of the cameras' sources, per user (HKCU\Software\Show2Cam\Camera<N>), and the settings file (export /
// import: UTF-16 INI). Shared by the panel, s2cctl and the autotest.
//   [Show2Cam]  Version, CameraCount
//   [Camera<N>] Name, Source, Text, ImageFolder, VideoFolder, Url, AudioMode, AudioDevice, Width, Height, Fps, Paused
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
void CamConfigNotifyPanel(int index);           // -1: all cameras
bool CamPanelRunning();

// Settings file. Writing: create (UTF-16 BOM), the header, every camera, flush (false: not written).
bool CamIniCreate(const wchar_t* path);
void CamIniWriteHeader(const wchar_t* path, int cameraCount);
void CamIniWriteCamera(const wchar_t* path, int index, const wchar_t* name, const CamConfig& c);
bool CamIniFlush(const wchar_t* path);
// Reading. CamIniCheck: false if the file is not Show2Cam settings; *count = CameraCount (0: not in the file).
bool CamIniCheck(const wchar_t* path, int* count);
// Camera `index` (0-based) from the file over *c (values missing or invalid in the file keep *c). false: the file has
// no such camera. name: S2C_NAME_CHARS; *hasName = the file has a name ("" = the default name).
bool CamIniReadCamera(const wchar_t* path, int index, CamConfig* c, wchar_t* name, bool* hasName);
