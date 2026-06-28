#pragma once

#include <string>
#include <vector>

constexpr int kTaskbarBatteryStyleCount = 2;

struct AppConfig {
    int refreshIntervalSeconds = 10;
    int lowBatteryThreshold = 20;
    bool showDisconnectedDevices = false;
    bool enableLowBatteryNotify = true;
    bool enableConnectionNotify = true;
    bool startWithWindows = false;
    // Deprecated: kept only for compatibility with existing config files.
    bool showTaskbarBattery = true;
    int taskbarBatteryStyle = 0;
    int taskbarMaxDevices = 3;
    std::vector<std::wstring> pinnedDeviceIds;
};

class ConfigStore {
public:
    void Load();
    void Save() const;
    const AppConfig& Get() const { return config_; }
    AppConfig& Edit() { return config_; }
    std::wstring Path() const { return path_; }

private:
    AppConfig config_;
    std::wstring path_;
};
