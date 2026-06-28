#pragma once

#include "config.h"
#include "services/bluetooth_device_watcher.h"
#include "services/scanner_service.h"
#include "services/update_service.h"
#include "tray.h"

#include <Windows.h>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <vector>

class App {
public:
    int Run(HINSTANCE instance, int showCmd);

private:
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK SettingsWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK StatusWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK TrayPanelWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK DevicePanelWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK AboutWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK UpdateWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK ConnectionToastWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);
    LRESULT HandleMessage(UINT message, WPARAM wParam, LPARAM lParam);

    bool CreateHiddenWindow(HINSTANCE instance);
    bool CreateBatteryWindow();
    void SyncBatteryWindow();
    void StartTimer();
    bool RefreshAsync();
    void ScheduleBluetoothChangeRefresh(const std::wstring& source);
    void RefreshFromBluetoothChange();
    void UpdateTaskbarDisplayCache();
    std::vector<BluetoothDeviceInfo> TaskbarDisplayDevices() const;
    void ClearDeviceCache();
    void ApplyScanResult(std::vector<BluetoothDeviceInfo>* result);
    void UpdateTray();
    void UpdateStatusWindow();
    void PositionStatusWindow();
    bool IsOtherApplicationFullscreen() const;
    void PaintStatusWindow(HWND hwnd);
    HWND FindTaskbarWindow() const;
    HWND FindTrayNotifyWindow(HWND taskbar) const;
    std::wstring BuildScanSummaryText() const;
    void PaintSettingsWindow(HWND hwnd);
    void LayoutSettingsControls(HWND hwnd);
    void SetSettingsSection(HWND hwnd, int section);
    void ShowTrayPanel();
    void CloseTrayPanels();
    void ShowDevicePanel();
    bool IsPointInsideTrayPanels(POINT pt) const;
    void PaintTrayPanel(HWND hwnd);
    void PaintDevicePanel(HWND hwnd);
    void HandleTrayPanelClick(HWND hwnd, int x, int y);
    void HandleTrayPanelMouse(HWND hwnd, int x, int y);
    void HandleDevicePanelMouse(HWND hwnd, int x, int y);
    void HandleDevicePanelClick(HWND hwnd, int x, int y);
    void CheckLowBattery();
    void TrackConnectionChanges(const std::vector<BluetoothDeviceInfo>& newDevices);
    void QueueConnectionToast(const std::wstring& text);
    void ShowNextConnectionToast();
    bool CreateConnectionToastWindow();
    void CloseConnectionToastWindow();
    void PaintConnectionToastWindow(HWND hwnd);
    void ShowSettings();
    void ShowAbout();
    void PaintAboutWindow(HWND hwnd);
    void HandleAboutClick(HWND hwnd, int x, int y);
    void ShowUpdateDialog(bool startCheck);
    void PaintUpdateWindow(HWND hwnd);
    void HandleUpdateClick(HWND hwnd, int x, int y);
    void CheckForUpdatesAsync();
    void ToggleStartup();
    void Shutdown();

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    HWND statusWindow_ = nullptr;
    HWND settingsWindow_ = nullptr;
    HWND trayPanelWindow_ = nullptr;
    HWND devicePanelWindow_ = nullptr;
    HWND aboutWindow_ = nullptr;
    HWND updateWindow_ = nullptr;
    HWND connectionToastWindow_ = nullptr;
    HWND taskbarWindow_ = nullptr;
    HFONT settingsControlFont_ = nullptr;
    UINT taskbarCreatedMessage_ = 0;
    int activeSettingsSection_ = 0;
    int settingsScrollY_ = 0;
    int trayPanelHoverItem_ = -1;
    int devicePanelHoverItem_ = -1;
    int updateStatus_ = 0;
    std::wstring updateTitle_;
    std::wstring updateMessage_;
    std::wstring updatePrimaryText_;
    std::wstring updatePrimaryUrl_;
    bool positioningStatusWindow_ = false;
    bool scanInProgress_ = false;
    DWORD lastScanTick_ = 0;
    int lastScanDeviceCount_ = 0;
    int lastScanBatteryCount_ = 0;
    bool hasScanBaseline_ = false;
    std::map<std::wstring, bool> previousConnectionStates_;
    std::map<std::wstring, DWORD> recentConnectionNotifyTicks_;
    std::deque<std::wstring> connectionToastQueue_;
    std::wstring currentConnectionToastText_;
    TrayIcon tray_;
    ConfigStore configStore_;
    BluetoothDeviceWatcherService bluetoothWatcher_;
    ScannerService scannerService_;
    UpdateService updateService_;
    std::vector<BluetoothDeviceInfo> devices_;
    std::vector<BluetoothDeviceInfo> taskbarDisplayCache_;
    DWORD taskbarDisplayCacheTick_ = 0;
    int taskbarEmptyScanCount_ = 0;
    std::set<std::wstring> lowBatteryNotified_;
};
