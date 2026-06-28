#include "services/scanner_service.h"

#include "logger.h"
#include "tray.h"

bool ScannerService::RefreshAsync(HWND target, const AppConfig& config) {
    bool expected = false;
    if (!scanning_.compare_exchange_strong(expected, true)) {
        Logger::Instance().Warn(L"上一轮扫描未结束，跳过本次刷新");
        return false;
    }
    if (scanThread_.joinable()) {
        scanThread_.join();
    }
    scanThread_ = std::thread([this, target, config]() {
        auto* result = new std::vector<BluetoothDeviceInfo>(scanner_.Scan(config));
        PostMessageW(target, WM_SCAN_COMPLETE, 0, reinterpret_cast<LPARAM>(result));
    });
    return true;
}

void ScannerService::CompleteScan() {
    scanning_ = false;
    if (scanThread_.joinable()) {
        scanThread_.join();
    }
}

void ScannerService::Shutdown() {
    if (scanThread_.joinable()) {
        scanThread_.join();
    }
}
