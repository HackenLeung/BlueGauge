#pragma once

#include "bluetooth/bluetooth_types.h"
#include "config.h"

#include <vector>

class BluetoothScanner {
public:
    std::vector<BluetoothDeviceInfo> Scan(const AppConfig& config);
};

