#pragma once

#include "bluetooth/bluetooth_types.h"

#include <string>

BluetoothConnectionKind GetClassicBluetoothConnectionStatus(
    const std::wstring& instanceId,
    std::wstring* error
);

BluetoothConnectionKind GetBleBluetoothConnectionStatus(
    const std::wstring& instanceId,
    std::wstring* error
);
