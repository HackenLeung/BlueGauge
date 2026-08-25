#pragma once

#include <optional>
#include <string>

enum class BluetoothConnectionKind {
    Disconnected,
    Connected,
    Unknown
};

struct BluetoothDeviceInfo {
    std::wstring id;
    // 实际显示名：默认等于 systemName，配置里有别名时被替换。
    std::wstring name;
    // 扫描到的原始名字，用于重命名弹窗显示原名和恢复默认。
    std::wstring systemName;
    bool connected = false;
    std::optional<int> batteryPercent;
    std::wstring source;
};
