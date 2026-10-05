// The "cannot work" notice for S2mCheckReady() in the interface language (programs that use lang.h).
#pragma once

#include "devctl.h"
#include "lang.h"

inline const wchar_t* S2cNotReadyText(S2cNotReady reason)
{
    switch (reason)
    {
    case S2cNotReadySecureBoot:
        return TR(L"Show2Cam не может работать: включён Secure Boot, а драйвер Show2Cam с тестовой подписью при нём не загружается.\n\nВыключите Secure Boot в настройках UEFI (VMware: VM → Settings → Options → Advanced), затем запустите Show2Cam-Setup.exe.");
    case S2cNotReadyTestMode:
        return TR(L"Show2Cam не может работать: тестовый режим подписи Windows выключен, без него драйвер Show2Cam не загружается.\n\nЗапустите Show2Cam-Setup.exe, нажмите «Включить тестовый режим» и перезагрузите компьютер.");
    case S2cNotReadyDriver:
        return TR(L"Show2Cam не может работать: драйвер Show2Cam не установлен.\n\nЗапустите Show2Cam-Setup.exe и нажмите «Установить».");
    default:
        return L"";
    }
}
