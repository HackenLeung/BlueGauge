#pragma once

#include "bluetooth/bluetooth_types.h"

#include <Windows.h>
#include <vector>

constexpr UINT kTrayIconId = 1;
constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_SCAN_COMPLETE = WM_APP + 2;
constexpr UINT WM_UPDATE_RESULT = WM_APP + 3;
constexpr UINT WM_BLUETOOTH_DEVICE_CHANGED = WM_APP + 4;

constexpr UINT IDM_REFRESH = 2001;
constexpr UINT IDM_SETTINGS = 2002;
constexpr UINT IDM_CHECK_UPDATE = 2003;
constexpr UINT IDM_ABOUT = 2004;
constexpr UINT IDM_EXIT = 2005;
constexpr UINT IDM_DEVICE_BASE = 2100;

enum TrayIconState {
    TrayDisconnected,
    TrayNormal,
    TrayWarning
};

class TrayIcon {
public:
    bool Add(HWND hwnd, HINSTANCE instance);
    void Remove();
    void Update(TrayIconState state, const std::vector<BluetoothDeviceInfo>& devices);
    void ShowMenu(HWND hwnd, const std::vector<BluetoothDeviceInfo>& devices, bool startupEnabled);

private:
    HICON LoadStateIcon(TrayIconState state) const;
    std::wstring BuildTooltip(const std::vector<BluetoothDeviceInfo>& devices) const;

    HWND hwnd_ = nullptr;
    HINSTANCE instance_ = nullptr;
    bool added_ = false;
};
