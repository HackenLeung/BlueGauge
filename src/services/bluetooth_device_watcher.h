#pragma once

#include <Windows.h>
#include <memory>

class BluetoothDeviceWatcherService {
public:
    BluetoothDeviceWatcherService();
    ~BluetoothDeviceWatcherService();

    BluetoothDeviceWatcherService(const BluetoothDeviceWatcherService&) = delete;
    BluetoothDeviceWatcherService& operator=(const BluetoothDeviceWatcherService&) = delete;

    bool Start(HWND target, UINT message);
    void Stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
