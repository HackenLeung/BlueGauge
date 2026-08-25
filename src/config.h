#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

constexpr int kTaskbarBatteryStyleCount = 2;
// 别名条目上限，够用且配置文件不会无限长。
constexpr size_t kMaxDeviceAliases = 32;

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
    // 设备 id -> 自定义显示名。缺失时用扫描到的系统名。
    std::map<std::wstring, std::wstring> deviceAliases;
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
