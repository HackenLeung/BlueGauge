#include "config.h"

#include <ShlObj.h>
#include <Windows.h>
#include <KnownFolders.h>
#include <algorithm>
#include <filesystem>

namespace {
constexpr int kConfigVersion = 3;
constexpr int kMaxPinnedDevices = 3;

int ClampInt(int value, int minValue, int maxValue) {
    return std::max(minValue, std::min(value, maxValue));
}

std::wstring ReadProfileString(const wchar_t* section, const wchar_t* key, const std::wstring& path) {
    wchar_t buffer[512]{};
    GetPrivateProfileStringW(section, key, L"", buffer, 512, path.c_str());
    return buffer;
}
}

void ConfigStore::Load() {
    PWSTR appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        std::filesystem::path dir = std::filesystem::path(appData) / L"BlueGauge";
        CoTaskMemFree(appData);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        path_ = (dir / L"config.ini").wstring();
    }
    if (path_.empty()) {
        return;
    }

    config_.refreshIntervalSeconds = ClampInt(GetPrivateProfileIntW(L"General", L"RefreshIntervalSeconds", 10, path_.c_str()), 5, 3600);
    config_.lowBatteryThreshold = ClampInt(GetPrivateProfileIntW(L"General", L"LowBatteryThreshold", 20, path_.c_str()), 1, 100);
    config_.showDisconnectedDevices = GetPrivateProfileIntW(L"General", L"ShowDisconnectedDevices", 0, path_.c_str()) != 0;
    config_.enableLowBatteryNotify = GetPrivateProfileIntW(L"General", L"EnableLowBatteryNotify", 1, path_.c_str()) != 0;
    config_.enableConnectionNotify = GetPrivateProfileIntW(L"General", L"EnableConnectionNotify", 1, path_.c_str()) != 0;
    config_.startWithWindows = GetPrivateProfileIntW(L"General", L"StartWithWindows", 0, path_.c_str()) != 0;
    const int configVersion = GetPrivateProfileIntW(L"General", L"ConfigVersion", 0, path_.c_str());
    const int taskbarBatteryValue = GetPrivateProfileIntW(L"General", L"ShowTaskbarBattery", 1, path_.c_str());
    config_.showTaskbarBattery = configVersion < 2 ? true : taskbarBatteryValue != 0;
    const int storedTaskbarStyle = GetPrivateProfileIntW(L"General", L"TaskbarBatteryStyle", 0, path_.c_str());
    config_.taskbarBatteryStyle = configVersion < kConfigVersion
        ? (storedTaskbarStyle >= 2 ? 1 : 0)
        : ClampInt(storedTaskbarStyle, 0, kTaskbarBatteryStyleCount - 1);
    config_.taskbarMaxDevices = ClampInt(GetPrivateProfileIntW(L"General", L"TaskbarMaxDevices", 3, path_.c_str()), 1, 3);
    config_.pinnedDeviceIds.clear();
    for (int i = 1; i <= kMaxPinnedDevices; ++i) {
        const std::wstring key = L"Device" + std::to_wstring(i);
        std::wstring id = ReadProfileString(L"PinnedDevices", key.c_str(), path_);
        if (!id.empty() && std::find(config_.pinnedDeviceIds.begin(), config_.pinnedDeviceIds.end(), id) == config_.pinnedDeviceIds.end()) {
            config_.pinnedDeviceIds.push_back(id);
        }
    }
}

void ConfigStore::Save() const {
    if (path_.empty()) {
        return;
    }
    WritePrivateProfileStringW(L"General", L"RefreshIntervalSeconds", std::to_wstring(config_.refreshIntervalSeconds).c_str(), path_.c_str());
    WritePrivateProfileStringW(L"General", L"LowBatteryThreshold", std::to_wstring(config_.lowBatteryThreshold).c_str(), path_.c_str());
    WritePrivateProfileStringW(L"General", L"ShowDisconnectedDevices", config_.showDisconnectedDevices ? L"1" : L"0", path_.c_str());
    WritePrivateProfileStringW(L"General", L"EnableLowBatteryNotify", config_.enableLowBatteryNotify ? L"1" : L"0", path_.c_str());
    WritePrivateProfileStringW(L"General", L"EnableConnectionNotify", config_.enableConnectionNotify ? L"1" : L"0", path_.c_str());
    WritePrivateProfileStringW(L"General", L"StartWithWindows", config_.startWithWindows ? L"1" : L"0", path_.c_str());
    WritePrivateProfileStringW(L"General", L"ShowTaskbarBattery", config_.showTaskbarBattery ? L"1" : L"0", path_.c_str());
    WritePrivateProfileStringW(L"General", L"TaskbarBatteryStyle", std::to_wstring(config_.taskbarBatteryStyle).c_str(), path_.c_str());
    WritePrivateProfileStringW(L"General", L"TaskbarMaxDevices", std::to_wstring(config_.taskbarMaxDevices).c_str(), path_.c_str());
    for (int i = 1; i <= kMaxPinnedDevices; ++i) {
        const std::wstring key = L"Device" + std::to_wstring(i);
        const wchar_t* value = i <= static_cast<int>(config_.pinnedDeviceIds.size()) ? config_.pinnedDeviceIds[static_cast<size_t>(i - 1)].c_str() : L"";
        WritePrivateProfileStringW(L"PinnedDevices", key.c_str(), value, path_.c_str());
    }
    WritePrivateProfileStringW(L"General", L"ConfigVersion", std::to_wstring(kConfigVersion).c_str(), path_.c_str());
}
