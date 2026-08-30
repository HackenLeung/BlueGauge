#pragma once

#include "bluetooth/bluetooth_types.h"

#include <vector>

// 扫描 XCTECH 方案的 2.4G / USB 鼠标（英菲克 IN9、A9 等）。
// 协议是厂商私有的 33 字节 HID 报文，只发只读的 GetDeviceInfo(0x10)。
// 只返回握手成功且校验和正确的设备，休眠或未配对的设备不会出现在结果里。
std::vector<BluetoothDeviceInfo> ScanXctechDevices();
