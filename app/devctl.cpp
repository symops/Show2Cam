// Show2Cam device control (see devctl.h).
#include "devctl.h"
#include "applog.h"
#include <devpropdef.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <wchar.h>

// {4d36e96c-e325-11ce-bfc1-08002be10318}: "Sound, video and game controllers"

static bool ChangeState(HDEVINFO set, SP_DEVINFO_DATA* info, DWORD state, bool* reboot)
{
    SP_PROPCHANGE_PARAMS pc = {};
    pc.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
    pc.ClassInstallHeader.InstallFunction = DIF_PROPERTYCHANGE;
    pc.StateChange = state;
    pc.Scope = DICS_FLAG_GLOBAL;
    bool ok = SetupDiSetClassInstallParamsW(set, info, &pc.ClassInstallHeader, sizeof(pc)) &&
              SetupDiCallClassInstaller(DIF_PROPERTYCHANGE, set, info);
    if (!ok) AppLog(L"restart: state change %lu failed, error %lu", state, GetLastError());

    SP_DEVINSTALL_PARAMS_W dip = {};
    dip.cbSize = sizeof(dip);
    if (SetupDiGetDeviceInstallParamsW(set, info, &dip) && (dip.Flags & (DI_NEEDREBOOT | DI_NEEDRESTART)))
    {
        *reboot = true;
    }
    return ok;
}

bool S2cRestartDevice(bool* found, bool* reboot)
{
    *found = false;
    *reboot = false;
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE)
    {
        return false;
    }

    bool ok = true;
    SP_DEVINFO_DATA info;
    info.cbSize = sizeof(info);
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); i++)
    {
        wchar_t ids[1024] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_HARDWAREID, nullptr, (BYTE*)ids, sizeof(ids) - 4, nullptr))
        {
            continue;
        }
        bool match = false;
        for (const wchar_t* p = ids; *p; p += wcslen(p) + 1)
        {
            if (_wcsicmp(p, S2C_HARDWARE_ID) == 0) match = true;
        }
        if (!match)
        {
            continue;
        }

        *found = true;
        if (!ChangeState(set, &info, DICS_DISABLE, reboot)) ok = false;
        if (!ChangeState(set, &info, DICS_ENABLE, reboot)) ok = false;
        AppLog(L"restart: disable + enable %ls%ls", ok ? L"done" : L"FAILED", *reboot ? L", reboot required" : L"");
    }
    SetupDiDestroyDeviceInfoList(set);
    return ok;
}

DWORD S2cGetParam(const wchar_t* name, DWORD def)
{
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, S2C_PARAMS_KEY, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS)
        return def;
    return value;
}

bool S2cSetParam(const wchar_t* name, DWORD value)
{
    HKEY key;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, S2C_PARAMS_KEY, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return false;
    bool ok = RegSetValueExW(key, name, 0, REG_DWORD, (const BYTE*)&value, sizeof(value)) == ERROR_SUCCESS;
    RegCloseKey(key);
    return ok;
}

// Device Manager name of a device node: DEVPKEY_Device_FriendlyName (endpoint nodes created by the audio
// service keep it only in the property store, SPDRP_FRIENDLYNAME fails for them), else the description.

// ---------------------------------------------------------------------------
// Ready to work?

bool S2cSecureBootEnabled()
{
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\SecureBoot\\State",
                        L"UEFISecureBootEnabled", RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v != 0;
}

bool S2cTestSigningEnabled()
{
    wchar_t opts[1024] = {};
    DWORD size = sizeof(opts);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control", L"SystemStartOptions",
                     RRF_RT_REG_SZ, nullptr, opts, &size) != ERROR_SUCCESS)
    {
        return false;
    }
    _wcsupr(opts);
    return wcsstr(opts, L"TESTSIGNING") != nullptr;
}

bool S2cDriverInstalled()
{
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES | DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    SP_DEVINFO_DATA info;
    info.cbSize = sizeof(info);
    for (DWORD i = 0; !found && SetupDiEnumDeviceInfo(set, i, &info); i++)
    {
        wchar_t ids[1024] = {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_HARDWAREID, nullptr, (BYTE*)ids, sizeof(ids) - 4, nullptr))
            continue;
        for (const wchar_t* p = ids; *p; p += wcslen(p) + 1)
            if (_wcsicmp(p, S2C_HARDWARE_ID) == 0) found = true;
    }
    SetupDiDestroyDeviceInfoList(set);
    return found;
}

S2cNotReady S2cCheckReady()
{
    bool secure = S2cSecureBootEnabled(), test = S2cTestSigningEnabled(), driver = S2cDriverInstalled();
    S2cNotReady r = secure ? S2cNotReadySecureBoot : !test ? S2cNotReadyTestMode : !driver ? S2cNotReadyDriver : S2cReadyOk;
    if (r != S2cReadyOk)
        AppLog(L"not ready to work: Secure Boot %ls, test signing %ls, driver %ls", secure ? L"ON" : L"off", test ? L"on" : L"OFF",
               driver ? L"installed" : L"NOT installed");
    return r;
}

const wchar_t* S2cNotReadyTextEn(S2cNotReady reason)
{
    switch (reason)
    {
    case S2cNotReadySecureBoot:
        return L"Show2Cam cannot work: Secure Boot is on, and the test-signed Show2Cam driver does not load with it. "
               L"Turn Secure Boot off in the UEFI settings, then run Show2Cam-Setup.exe.";
    case S2cNotReadyTestMode:
        return L"Show2Cam cannot work: Windows test signing mode is off, and the Show2Cam driver does not load without it. "
               L"Run Show2Cam-Setup.exe, click \"Enable test mode\" and restart the computer.";
    case S2cNotReadyDriver:
        return L"Show2Cam cannot work: the Show2Cam driver is not installed. Run Show2Cam-Setup.exe and click \"Install\".";
    default:
        return L"";
    }
}
