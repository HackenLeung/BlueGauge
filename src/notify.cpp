#include "notify.h"

#include "tray.h"

#include <Shellapi.h>

void ShowBalloon(HWND hwnd, const std::wstring& title, const std::wstring& message) {
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd;
    data.uID = kTrayIconId;
    data.uFlags = NIF_INFO;
    wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, message.c_str(), _TRUNCATE);
    data.dwInfoFlags = NIIF_WARNING;
    Shell_NotifyIconW(NIM_MODIFY, &data);
}
