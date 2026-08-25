#pragma once

#include "bluetooth/bluetooth_types.h"

#include <vector>

// 扫描通过罗技接收器（Lightspeed / Unifying / Bolt）连接的 2.4G 设备。
// 只返回 HID++ ping 有应答的设备，未配对或已休眠的设备不会出现在结果里。
std::vector<BluetoothDeviceInfo> ScanLogitechHidppDevices();
