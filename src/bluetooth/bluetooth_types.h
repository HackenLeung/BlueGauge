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
    std::wstring name;
    bool connected = false;
    std::optional<int> batteryPercent;
    std::wstring source;
};
