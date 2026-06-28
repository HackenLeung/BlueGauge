#include "bluetooth/bluetooth_scanner.h"

#include "bluetooth/ble_battery.h"
#include "bluetooth/btc_battery.h"
#include "bluetooth/winrt_bluetooth_status.h"
#include "logger.h"

#include <Windows.h>
#include <SetupAPI.h>
#include <devguid.h>
#include <algorithm>
#include <cwctype>
#include <memory>
#include <sstream>

namespace {
struct DevInfoHandleDeleter {
    void operator()(HDEVINFO handle) const {
        if (handle != INVALID_HANDLE_VALUE) {
            SetupDiDestroyDeviceInfoList(handle);
        }
    }
};
using DevInfoHandle = std::unique_ptr<std::remove_pointer_t<HDEVINFO>, DevInfoHandleDeleter>;

std::wstring GetDeviceString(HDEVINFO set, SP_DEVINFO_DATA& data, DWORD property) {
    wchar_t buffer[512]{};
    DWORD type = 0;
    DWORD size = 0;
    if (SetupDiGetDeviceRegistryPropertyW(set, &data, property, &type, reinterpret_cast<PBYTE>(buffer), sizeof(buffer), &size)) {
        return buffer;
    }
    return {};
}

std::wstring GetInstanceId(HDEVINFO set, SP_DEVINFO_DATA& data) {
    wchar_t buffer[512]{};
    if (SetupDiGetDeviceInstanceIdW(set, &data, buffer, static_cast<DWORD>(std::size(buffer)), nullptr)) {
        return buffer;
    }
    return {};
}

std::wstring LastErrorText(DWORD error) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<LPWSTR>(&message), 0, nullptr);
    std::wstring text = message ? message : L"未知错误";
    if (message) {
        LocalFree(message);
    }
    return text;
}

bool StartsWithIgnoreCase(const std::wstring& text, const std::wstring& prefix) {
    if (text.size() < prefix.size()) {
        return false;
    }
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (std::towupper(text[i]) != std::towupper(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool IsTopLevelBluetoothDevice(const std::wstring& instanceId) {
    return StartsWithIgnoreCase(instanceId, L"BTHLE\\DEV_")
        || StartsWithIgnoreCase(instanceId, L"BTHENUM\\DEV_");
}

bool IsBleTopLevelDevice(const std::wstring& instanceId) {
    return StartsWithIgnoreCase(instanceId, L"BTHLE\\DEV_");
}

std::wstring ConnectionKindText(BluetoothConnectionKind kind) {
    switch (kind) {
    case BluetoothConnectionKind::Connected:
        return L"Connected";
    case BluetoothConnectionKind::Disconnected:
        return L"Disconnected";
    default:
        return L"Unknown";
    }
}
}

std::vector<BluetoothDeviceInfo> BluetoothScanner::Scan(const AppConfig& config) {
    (void)config;
    Logger::Instance().Info(L"蓝牙扫描开始");
    std::vector<BluetoothDeviceInfo> devices;

    DevInfoHandle set(SetupDiGetClassDevsW(&GUID_DEVCLASS_BLUETOOTH, nullptr, nullptr, DIGCF_PRESENT));
    if (set.get() == INVALID_HANDLE_VALUE) {
        Logger::Instance().Error(L"SetupDiGetClassDevsW 失败: " + LastErrorText(GetLastError()));
        return devices;
    }

    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA data{};
        data.cbSize = sizeof(data);
        if (!SetupDiEnumDeviceInfo(set.get(), index, &data)) {
            if (GetLastError() != ERROR_NO_MORE_ITEMS) {
                Logger::Instance().Warn(L"SetupDiEnumDeviceInfo 失败: " + LastErrorText(GetLastError()));
            }
            break;
        }

        BluetoothDeviceInfo device;
        device.id = GetInstanceId(set.get(), data);
        if (!IsTopLevelBluetoothDevice(device.id)) {
            continue;
        }
        device.name = GetDeviceString(set.get(), data, SPDRP_FRIENDLYNAME);
        if (device.name.empty()) {
            device.name = GetDeviceString(set.get(), data, SPDRP_DEVICEDESC);
        }
        if (device.name.empty()) {
            device.name = L"未知蓝牙设备";
        }
        const bool isBle = IsBleTopLevelDevice(device.id);
        device.source = isBle ? L"BLE" : L"Classic";

        std::wstring statusDetail;
        const BluetoothConnectionKind status = isBle
            ? GetBleBluetoothConnectionStatus(device.id, &statusDetail)
            : GetClassicBluetoothConnectionStatus(device.id, &statusDetail);
        device.connected = status == BluetoothConnectionKind::Connected;

        std::wstring statusLog = device.name + L" WinRT 状态: " + ConnectionKindText(status);
        if (!statusDetail.empty()) {
            statusLog += L" (" + statusDetail + L")";
        }
        if (!device.connected) {
            Logger::Instance().Info(statusLog + L"，跳过电量读取");
            devices.push_back(device);
            continue;
        }

        Logger::Instance().Info(statusLog + L"，开始读取电量");

        std::wstring error;
        if (isBle) {
            device.batteryPercent = TryReadBleBatteryLevel(device.id, error);
            device.source = L"BLE";
            if (!device.batteryPercent.has_value()) {
                Logger::Instance().Warn(device.name + L" BLE 电量读取失败: " + error);
            }
        } else {
            device.batteryPercent = TryReadClassicBluetoothBattery(device.id, error);
            device.source = L"Classic";
            if (!device.batteryPercent.has_value()) {
                Logger::Instance().Warn(device.name + L" 经典蓝牙电量读取失败: " + error);
            }
        }

        if (!device.batteryPercent.has_value()) {
            device.source = L"Unknown";
        } else {
            Logger::Instance().Info(device.name + L" 电量读取成功: " + std::to_wstring(*device.batteryPercent) + L"%");
        }

        devices.push_back(device);
    }

    std::sort(devices.begin(), devices.end(), [](const auto& a, const auto& b) {
        if (a.connected != b.connected) {
            return a.connected > b.connected;
        }
        return a.name < b.name;
    });

    Logger::Instance().Info(L"蓝牙扫描结束，设备数量: " + std::to_wstring(devices.size()));
    return devices;
}
