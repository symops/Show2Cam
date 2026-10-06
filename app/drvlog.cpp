// driver.log writer of the control panel (see camdev.h).
#include "camdev.h"
#include "applog.h"
#include <stdlib.h>
#include <wchar.h>


static HANDLE        g_logThread, g_logStop;
static ULONG         g_logGeneration;

static void WriteDriverLog(const S2C_LOG* l)
{
    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%ls\\driver.log", AppLogDir());
    path[MAX_PATH - 1] = 0;
    char* text = (char*)malloc((size_t)l->Length * 2 + 2);
    if (!text) return;
    DWORD n = 0;
    for (ULONG i = 0; i < l->Length; i++)
    {
        if (l->Text[i] == '\n' && (i == 0 || l->Text[i - 1] != '\r')) text[n++] = '\r';
        text[n++] = l->Text[i];
    }
    wchar_t tmp[MAX_PATH];
    _snwprintf(tmp, MAX_PATH, L"%ls.new", path);
    tmp[MAX_PATH - 1] = 0;
    HANDLE f = CreateFileW(tmp, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f != INVALID_HANDLE_VALUE)
    {
        DWORD w = 0;
        bool ok = WriteFile(f, text, n, &w, nullptr) && w == n;
        CloseHandle(f);
        if (ok) MoveFileExW(tmp, path, MOVEFILE_REPLACE_EXISTING);
        else DeleteFileW(tmp);
    }
    free(text);
}

static DWORD WINAPI DriverLogThread(LPVOID)
{
    S2C_LOG* l = (S2C_LOG*)malloc(sizeof(S2C_LOG));
    if (!l) return 0;
    do
    {
        CamInfo cams[S2C_MAX_CAMERAS_UI];
        int n = CamList(cams, S2C_MAX_CAMERAS_UI);
        for (int i = 0; i < n; i++)
        {
            HANDLE h = CamOpen(cams[i].path);
            if (h == INVALID_HANDLE_VALUE) continue;
            bool ok = CamGetLog(h, l);
            CloseHandle(h);
            if (!ok) continue;
            if (l->Generation != g_logGeneration)
            {
                g_logGeneration = l->Generation;
                WriteDriverLog(l);
            }
            break;
        }
    } while (WaitForSingleObject(g_logStop, 2000) == WAIT_TIMEOUT);
    free(l);
    return 0;
}

void DriverLogWriterStart()
{
    if (g_logThread) return;
    g_logStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_logThread = CreateThread(nullptr, 0, DriverLogThread, nullptr, 0, nullptr);
}

void DriverLogWriterStop()
{
    if (!g_logThread) return;
    SetEvent(g_logStop);
    WaitForSingleObject(g_logThread, 5000);
    CloseHandle(g_logThread);
    CloseHandle(g_logStop);
    g_logThread = g_logStop = nullptr;
}
