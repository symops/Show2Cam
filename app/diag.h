// Diagnostics of an installed Show2Cam: device node, driver service, driver log, cameras.
// Summary lines go to `log` (shown to the user); details go only to the program log file (applog).
#pragma once

#include "setupcore.h"

enum DiagResult
{
    DiagNotInstalled,       // no ROOT\Show2Cam device
    DiagDeviceProblem,      // device exists but is not started (problem code)
    DiagNoCameras,          // driver runs, but Windows offers no Show2Cam camera
    DiagOk,                 // the cameras are there
};

DiagResult RunDiagnostics(SetupLog log, void* ctx);

// Version of the driver that last started (from its log), e.g. "1.0.279.19"; false if unknown.
bool DiagRunningDriverVersion(wchar_t* out, size_t len);
// Driver version of the Show2Cam.inf next to this program; false if there is none.
bool DiagPackageDriverVersion(wchar_t* out, size_t len);

// Waits up to `timeoutMs` for the device to start and its cameras to appear.
DiagResult WaitForCameras(DWORD timeoutMs);
