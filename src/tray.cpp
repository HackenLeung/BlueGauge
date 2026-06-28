#include "tray.h"

#include "build_info.h"
#include "resource.h"

#include <Shellapi.h>
#include <sstream>

namespace {
std::wstring CompactPathForTooltip(const std::wstring& path) {
    const std::wstring marker = L"\\out\\build\\";
    const size_t pos = path.find(marker);
    if (pos != std::wstring::npos) {
        return L"out\\build\\" + path.substr(pos + marker.size());
    }
    return path;
}

std::wstring DeviceLine(const BluetoothDeviceInfo& device) {
    std::wstringstream ss;
    ss << device.name << L" - ";
    if (device.batteryPercent.has_value()) {
        ss << *device.batteryPercent << L"%";
    } else {
        ss << L"未知";
    }
    if (!device.connected) {
        ss << L"（未连接）";
    }
    return ss.str();
}
}

bool TrayIcon::Add(HWND hwnd, HINSTANCE instance) {
    hwnd_ = hwnd;
    instance_ = instance;

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = WM_TRAYICON;
    data.hIcon = LoadStateIcon(TrayNormal);
    wcsncpy_s(data.szTip, L"蓝牙设备：暂无数据", _TRUNCATE);
    added_ = Shell_NotifyIconW(NIM_ADD, &data) == TRUE;
    if (added_) {
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
    return added_;
}

void TrayIcon::Remove() {
    if (!added_) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    added_ = false;
}

void TrayIcon::Update(TrayIconState state, const std::vector<BluetoothDeviceInfo>& devices) {
    if (!added_) {
        return;
    }
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kTrayIconId;
    data.uFlags = NIF_ICON | NIF_TIP;
    data.hIcon = LoadStateIcon(state);
    wcsncpy_s(data.szTip, BuildTooltip(devices).c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void TrayIcon::ShowMenu(HWND hwnd, const std::vector<BluetoothDeviceInfo>& devices, bool startupEnabled) {
    (void)startupEnabled;
    HMENU menu = CreatePopupMenu();
    HMENU deviceMenu = CreatePopupMenu();

    if (devices.empty()) {
        AppendMenuW(deviceMenu, MF_STRING | MF_GRAYED, IDM_DEVICE_BASE, L"暂无蓝牙设备");
    } else {
        UINT index = 0;
        for (const auto& device : devices) {
            AppendMenuW(deviceMenu, MF_STRING | MF_GRAYED, IDM_DEVICE_BASE + index, DeviceLine(device).c_str());
            ++index;
        }
    }

    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(deviceMenu), L"设备列表");
    AppendMenuW(menu, MF_STRING, IDM_REFRESH, L"刷新");
    AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"设置");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_CHECK_UPDATE, L"检查更新");
    AppendMenuW(menu, MF_STRING, IDM_ABOUT, L"关于");
    AppendMenuW(menu, MF_STRING, IDM_EXIT, L"退出");

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
}

HICON TrayIcon::LoadStateIcon(TrayIconState state) const {
    int id = IDI_DISCONNECTED;
    if (state == TrayNormal) {
        id = IDI_APP;
    } else if (state == TrayWarning) {
        id = IDI_APP_WARNING;
    }
    const UINT dpi = hwnd_ ? GetDpiForWindow(hwnd_) : USER_DEFAULT_SCREEN_DPI;
    const int iconSize = GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    return reinterpret_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(id), IMAGE_ICON, iconSize, iconSize, LR_DEFAULTCOLOR | LR_SHARED));
}

std::wstring TrayIcon::BuildTooltip(const std::vector<BluetoothDeviceInfo>& devices) const {
    std::wstringstream ss;
    ss << GetBuildSummary();
    const std::wstring exePath = GetExecutablePath();
    if (!exePath.empty()) {
        ss << L"\n路径：" << CompactPathForTooltip(exePath);
    }
    ss << L"\n蓝牙设备：";
    if (devices.empty()) {
        ss << L"\n暂无设备";
        return ss.str();
    }
    int count = 0;
    for (const auto& device : devices) {
        if (count >= 6) {
            ss << L"\n...";
            break;
        }
        ss << L"\n" << DeviceLine(device);
        ++count;
    }
    return ss.str();
}
