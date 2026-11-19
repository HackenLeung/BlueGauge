#pragma once

#include "bluetooth/bluetooth_types.h"

#include <vector>

// 扫描 MCHOSE（迈从）的 2.4G 接收器。
//
// 这家前后两代协议完全不通用，两代都要试：
//   键盘（41E4:2001 等）  20 字节报文集，输出 report 0x13，帧头 13 4A 01，
//                         校验是全部字节求和，电量在回包 [5]，充电在 [6] 的高 4 位。
//   鼠标（3837:xxxx 新家族）64 字节报文集，report 0x4D，命令 0x0900（设备信息），
//                         校验是 [2..7+len] 的异或，电量在回包 [12]。
// 两条都是只读查询，不写设备配置。休眠 / 关机时接收器不回有效数据，按未连接处理。
std::vector<BluetoothDeviceInfo> ScanMchoseDevices();
