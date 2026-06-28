#pragma once

#include "bluetooth/bluetooth_scanner.h"
#include "config.h"

#include <Windows.h>
#include <atomic>
#include <thread>

class ScannerService {
public:
    bool RefreshAsync(HWND target, const AppConfig& config);
    void CompleteScan();
    void Shutdown();

private:
    BluetoothScanner scanner_;
    std::atomic_bool scanning_ = false;
    std::thread scanThread_;
};
