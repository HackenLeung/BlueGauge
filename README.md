# SplusXBTMeter

Windows 10/11 原生轻量级蓝牙电量监控工具。程序使用 C++17 与 Win32 API 编写，常驻系统托盘，支持设备列表、刷新、低电量提醒、设置保存、开机自启、单实例和日志。

## 功能

- 系统托盘常驻，不显示主窗口
- 托盘 tooltip 显示蓝牙设备摘要
- 右键菜单：设备列表、立即刷新、设置、开机自启、关于、退出
- 后台线程刷新设备，防止刷新重入
- 低电量提醒，同一低电量区间只提醒一次
- INI 配置：刷新间隔、低电量阈值、显示未连接设备、低电量提醒、开机自启
- 日志：`%AppData%\SplusXBTMeter\logs\app.log`
- 单实例运行

## 蓝牙兼容说明

当前实现优先提供稳定的 Win32 托盘程序框架，并通过 SetupAPI / CfgMgr32 枚举系统蓝牙设备。经典蓝牙和 BLE Battery Service 电量读取接口已拆分到独立模块，调用失败会写入日志并显示“未知”，不会导致程序崩溃。不同厂商蓝牙设备在 Windows 上暴露电量的方式差异较大，后续可在 `src/bluetooth/btc_battery.cpp` 与 `src/bluetooth/ble_battery.cpp` 中按设备能力扩展。

## 编译

需要：

- Windows 10/11
- Visual Studio 2022 C++ 桌面开发工具
- CMake 3.20+

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

生成文件：

```text
build\Release\SplusXBTMeter.exe
```

## 运行

直接运行 `SplusXBTMeter.exe`。程序会进入系统托盘。再次运行时会提示程序已运行。

## 打包 Release exe

Release 配置默认使用 `/O2`、LTCG、`/OPT:REF`、`/OPT:ICF`，并静态链接 MSVC 运行库，资源和图标内嵌到 exe。普通构建目标体积通常可控制在 10MB 以内，实际体积取决于工具链版本和运行库链接方式。

