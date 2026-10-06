// Driver entry and the AVStream device: reads the settings and creates one filter factory per camera.
//
// Settings (HKLM\SYSTEM\CurrentControlSet\Services\Show2Cam\Parameters, read when the device starts):
//   CameraCount       1..10 (default 1)
//   Camera<N>Width    even, 160..3840 (default 1280)
//   Camera<N>Height   even, 120..2160 (default 720)
//   Camera<N>Fps      1..60 (default 30)
//   Camera<N>Name     REG_SZ, the camera's name (default "Show2Cam Camera N")
// One device per camera (Device Parameters\CameraIndex); size, rate and name are also changed at run time by the
// Show2Cam program through the camera's property set (S2C_PROPERTY_FORMAT / S2C_PROPERTY_NAME).
#include "common.h"
#include "log.h"
#include "version.h"

static ULONG ReadDword(PCWSTR name, ULONG def)
{
    ULONG value = def;
    RTL_QUERY_REGISTRY_TABLE t[2];
    RtlZeroMemory(t, sizeof(t));
    t[0].Flags = RTL_QUERY_REGISTRY_DIRECT | RTL_QUERY_REGISTRY_TYPECHECK;
    t[0].Name = (PWSTR)name;
    t[0].EntryContext = &value;
    t[0].DefaultType = (REG_DWORD << RTL_QUERY_REGISTRY_TYPECHECK_SHIFT) | REG_DWORD;
    t[0].DefaultData = &def;
    t[0].DefaultLength = sizeof(ULONG);
    RtlQueryRegistryValues(RTL_REGISTRY_SERVICES, L"Show2Cam\\Parameters", t, nullptr, nullptr);
    return value;
}

static ULONG Clamp(ULONG v, ULONG lo, ULONG hi) { return v < lo ? lo : (v > hi ? hi : v); }

static void FreeCameras(S2C_DEVICE* d)
{
    for (ULONG i = 0; i < S2C_MAX_CAMERAS; i++)
    {
        if (d->Cameras[i]) S2cCameraFree(d->Cameras[i]);
        d->Cameras[i] = nullptr;
    }
}

static NTSTATUS EnableCamera(PKSDEVICE Device, S2C_DEVICE* d, ULONG i, BOOLEAN on);

// The camera of this device: Device Parameters\CameraIndex (0-based), 0 when not set.
static ULONG ReadDeviceIndex(PKSDEVICE Device)
{
    ULONG index = 0;
    HANDLE key;
    if (NT_SUCCESS(IoOpenDeviceRegistryKey(Device->PhysicalDeviceObject, PLUGPLAY_REGKEY_DEVICE, KEY_READ, &key)))
    {
        UNICODE_STRING name;
        RtlInitUnicodeString(&name, L"CameraIndex");
        UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + sizeof(ULONG)];
        ULONG got = 0;
        if (NT_SUCCESS(ZwQueryValueKey(key, &name, KeyValuePartialInformation, buf, sizeof(buf), &got)))
        {
            auto* v = (KEY_VALUE_PARTIAL_INFORMATION*)buf;
            if (v->Type == REG_DWORD && v->DataLength == sizeof(ULONG)) index = *(ULONG*)v->Data;
        }
        ZwClose(key);
    }
    return index < S2C_MAX_CAMERAS ? index : 0;
}

static NTSTATUS S2C_CB DeviceStart(_In_ PKSDEVICE Device, _In_ PIRP Irp, _In_opt_ PCM_RESOURCE_LIST Translated,
                                   _In_opt_ PCM_RESOURCE_LIST Untranslated)
{
    UNREFERENCED_PARAMETER(Irp);
    UNREFERENCED_PARAMETER(Translated);
    UNREFERENCED_PARAMETER(Untranslated);
    S2C_DEVICE* d = (S2C_DEVICE*)Device->Context;
    if (!d)
    {
        d = (S2C_DEVICE*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(S2C_DEVICE), S2C_POOLTAG);
        if (!d) return STATUS_INSUFFICIENT_RESOURCES;
        Device->Context = d;
    }
    if (d->FactoriesCreated) return STATUS_SUCCESS;        // restarted after a stop: the cameras still exist

    // One device per camera (as one-camera drivers: programs such as SearchInform take a camera only when a device has
    // its name): which one is in the device's own settings (Device Parameters\CameraIndex, set by the installer).
    ULONG index = ReadDeviceIndex(Device);
    S2cLog("StartDevice: Show2Cam %s, camera %lu", S2C_VER_STR, index + 1);
    KsAcquireDevice(Device);
    NTSTATUS status = EnableCamera(Device, d, index, TRUE);
    KsReleaseDevice(Device);
    ULONG created = NT_SUCCESS(status) ? 1 : 0;
    d->CameraCount = created;
    d->FactoriesCreated = created > 0;
    S2cLog("StartDevice: camera %lu -> 0x%08lX", index + 1, (ULONG)status);
    S2cLogSetValue(L"StartStatus", (ULONG)status);
    S2cLogSetValue(L"CamerasCreated", created);
    S2cLogFlush();
    return status;
}

// One camera on (created the first time: filter factory, device interfaces, saved name, format cache) or off (its
// interfaces disabled: programs no longer list it; a filter open stays usable until closed). Device mutex held.
static NTSTATUS EnableCamera(PKSDEVICE Device, S2C_DEVICE* d, ULONG i, BOOLEAN on)
{
    S2C_CAMERA* c = d->Cameras[i];
    if (!c)
    {
        if (!on) return STATUS_SUCCESS;
        WCHAR name[24];
        RtlStringCchPrintfW(name, 24, L"Camera%luWidth", i + 1);
        ULONG w = Clamp(ReadDword(name, 1280), 160, 3840) & ~1u;
        RtlStringCchPrintfW(name, 24, L"Camera%luHeight", i + 1);
        ULONG h = Clamp(ReadDword(name, 720), 120, 2160) & ~1u;
        RtlStringCchPrintfW(name, 24, L"Camera%luFps", i + 1);
        ULONG fps = Clamp(ReadDword(name, 30), 1, 60);
        NTSTATUS status = S2cCameraCreate(i, w, h, fps, &c);
        if (!NT_SUCCESS(status)) return status;
        status = KsCreateFilterFactory(Device->FunctionalDeviceObject, &c->FilterDescriptor, c->RefString, nullptr, 0, nullptr,
                                       nullptr, &c->Factory);
        S2cLog("Camera %lu: %lux%lu %lu fps -> 0x%08lX", i + 1, w, h, fps, (ULONG)status);
        c->Pdo = Device->PhysicalDeviceObject;
        if (!NT_SUCCESS(status))
        {
            S2cCameraFree(c);
            return status;
        }
        d->Cameras[i] = c;
    }
    if (c->Enabled == on) return STATUS_SUCCESS;
    NTSTATUS status = KsFilterFactorySetDeviceClassesState(c->Factory, on);
    if (!NT_SUCCESS(status))
    {
        S2cLog("Camera %lu: turning %s failed (0x%08lX)", i + 1, on ? "on" : "off", (ULONG)status);
        return status;
    }
    c->Enabled = on;
    if (on)
    {
        S2cCameraApplySavedName(c);    // the INF put the default name back on an install
        // The FilterData registry cache of the formats: some DirectShow programs read it before opening a camera
        // and skip cameras without it.
        NTSTATUS cache = KsFilterFactoryUpdateCacheData(c->Factory, nullptr);
        if (!NT_SUCCESS(cache)) S2cLog("Camera %lu: FilterData not written (0x%08lX)", i + 1, (ULONG)cache);
    }
    else
        S2cLog("Camera %lu: off", i + 1);
    return STATUS_SUCCESS;
}

static void S2C_CB DeviceRemove(_In_ PKSDEVICE Device, _In_ PIRP Irp)
{
    UNREFERENCED_PARAMETER(Irp);
    S2C_DEVICE* d = (S2C_DEVICE*)Device->Context;
    S2cLog("RemoveDevice");
    S2cLogFlush();
    if (d)
    {
        // The filter factories belong to the device object and go with it; the cameras they point to stay valid
        // until here.
        FreeCameras(d);
        ExFreePoolWithTag(d, S2C_POOLTAG);
        Device->Context = nullptr;
    }
}

static const KSDEVICE_DISPATCH kDeviceDispatch = {
    nullptr,                                // Add
    (PFNKSDEVICEPNPSTART)DeviceStart,
    nullptr,                                // PostStart
    nullptr, nullptr, nullptr,              // QueryStop, CancelStop, Stop
    nullptr, nullptr,                       // QueryRemove, CancelRemove
    (PFNKSDEVICEIRPVOID)DeviceRemove,
    nullptr, nullptr, nullptr, nullptr, nullptr
};

static const KSDEVICE_DESCRIPTOR kDeviceDescriptor = {
    &kDeviceDispatch, 0, nullptr, 0
};

extern "C" NTSTATUS NTAPI DriverEntry(_In_ PDRIVER_OBJECT DriverObject, _In_ PUNICODE_STRING RegistryPath)
{
    S2cLogInit();
    S2cLog("DriverEntry: Show2Cam %s", S2C_VER_STR);
    NTSTATUS status = KsInitializeDriver(DriverObject, RegistryPath, &kDeviceDescriptor);
    S2cLog("KsInitializeDriver -> 0x%08lX", (ULONG)status);
    S2cLogFlush();
    return status;
}
