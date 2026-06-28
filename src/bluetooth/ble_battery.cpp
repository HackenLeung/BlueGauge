#include "bluetooth/ble_battery.h"

#include "logger.h"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cwctype>
#include <mutex>
#include <regex>
#include <memory>
#include <vector>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

namespace {
struct AsyncWaitState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
};

template <typename TOperation>
bool WaitForCompletion(TOperation const& operation, std::chrono::milliseconds timeout) {
    auto state = std::make_shared<AsyncWaitState>();
    operation.Completed([state](auto&&, winrt::Windows::Foundation::AsyncStatus) {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->done = true;
        }
        state->cv.notify_one();
    });

    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->cv.wait_for(lock, timeout, [&] { return state->done; })) {
        operation.Cancel();
        return false;
    }
    return operation.Status() == winrt::Windows::Foundation::AsyncStatus::Completed;
}
}

std::optional<int> TryReadBleBatteryLevel(const std::wstring& instanceId, std::wstring& error) {
namespace bluetooth = winrt::Windows::Devices::Bluetooth;
namespace enumeration = winrt::Windows::Devices::Enumeration;
namespace foundation = winrt::Windows::Foundation;
    namespace gatt = winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
    namespace streams = winrt::Windows::Storage::Streams;

    try {
        if (instanceId.rfind(L"BTHLE\\DEV_", 0) != 0 && instanceId.rfind(L"bthle\\dev_", 0) != 0) {
            error = L"不是 BLE 顶层设备";
            return std::nullopt;
        }

        static const std::wregex addressPattern(LR"((?:DEV_|_)([0-9A-Fa-f]{12})(?:\\|$))");
        std::wsmatch match;
        if (!std::regex_search(instanceId, match, addressPattern)) {
            error = L"未从 PnP InstanceId 解析到 BLE 设备地址";
            return std::nullopt;
        }

        std::wstring addressText = match[1].str();
        uint64_t address = 0;
        for (wchar_t ch : addressText) {
            address <<= 4;
            if (ch >= L'0' && ch <= L'9') {
                address += static_cast<uint64_t>(ch - L'0');
            } else {
                address += static_cast<uint64_t>(std::towupper(ch) - L'A' + 10);
            }
        }

        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        constexpr auto timeout = std::chrono::milliseconds(2500);
        auto selector = bluetooth::BluetoothLEDevice::GetDeviceSelector();
        auto findOp = enumeration::DeviceInformation::FindAllAsync(selector);
        if (!WaitForCompletion(findOp, timeout)) {
            error = L"查找 BLE DeviceInformation 超时";
            return std::nullopt;
        }
        auto infos = findOp.GetResults();

        bluetooth::BluetoothLEDevice device{ nullptr };
        for (uint32_t index = 0; index < infos.Size(); ++index) {
            auto fromIdOp = bluetooth::BluetoothLEDevice::FromIdAsync(infos.GetAt(index).Id());
            if (!WaitForCompletion(fromIdOp, std::chrono::milliseconds(1500))) {
                continue;
            }
            auto candidate = fromIdOp.GetResults();
            if (candidate && candidate.BluetoothAddress() == address) {
                device = candidate;
                break;
            }
        }
        if (!device) {
            auto publicOp = bluetooth::BluetoothLEDevice::FromBluetoothAddressAsync(address, bluetooth::BluetoothAddressType::Public);
            if (WaitForCompletion(publicOp, timeout)) {
                device = publicOp.GetResults();
            }
        }
        if (!device) {
            auto randomOp = bluetooth::BluetoothLEDevice::FromBluetoothAddressAsync(address, bluetooth::BluetoothAddressType::Random);
            if (WaitForCompletion(randomOp, timeout)) {
                device = randomOp.GetResults();
            }
        }
        if (!device) {
            error = L"无法通过 DeviceInformation 或 BluetoothAddress 打开 BLE 设备";
            return std::nullopt;
        }
        if (device.ConnectionStatus() != bluetooth::BluetoothConnectionStatus::Connected) {
            error = L"BLE 设备当前未连接";
            return std::nullopt;
        }

        const winrt::guid batteryServiceUuid{ 0x0000180f, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb } };
        const winrt::guid batteryLevelUuid{ 0x00002a19, 0x0000, 0x1000, { 0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb } };

        auto servicesOp = device.GetGattServicesForUuidAsync(batteryServiceUuid, bluetooth::BluetoothCacheMode::Cached);
        if (!WaitForCompletion(servicesOp, timeout)) {
            error = L"读取 BLE Battery Service 超时";
            return std::nullopt;
        }
        auto servicesResult = servicesOp.GetResults();
        if (servicesResult.Status() != gatt::GattCommunicationStatus::Success || servicesResult.Services().Size() == 0) {
            error = L"未读取到 BLE Battery Service，状态: " + std::to_wstring(static_cast<int>(servicesResult.Status()));
            return std::nullopt;
        }

        auto service = servicesResult.Services().GetAt(0);
        auto charsOp = service.GetCharacteristicsForUuidAsync(batteryLevelUuid, bluetooth::BluetoothCacheMode::Cached);
        if (!WaitForCompletion(charsOp, timeout)) {
            error = L"读取 Battery Level 特征超时";
            return std::nullopt;
        }
        auto charsResult = charsOp.GetResults();
        if (charsResult.Status() != gatt::GattCommunicationStatus::Success || charsResult.Characteristics().Size() == 0) {
            error = L"未读取到 Battery Level 特征，状态: " + std::to_wstring(static_cast<int>(charsResult.Status()));
            return std::nullopt;
        }

        auto characteristic = charsResult.Characteristics().GetAt(0);
        auto readOp = characteristic.ReadValueAsync(bluetooth::BluetoothCacheMode::Cached);
        if (!WaitForCompletion(readOp, timeout)) {
            error = L"读取 Battery Level 数值超时";
            return std::nullopt;
        }
        auto readResult = readOp.GetResults();
        if (readResult.Status() != gatt::GattCommunicationStatus::Success || readResult.Value().Length() == 0) {
            error = L"读取 Battery Level 失败，状态: " + std::to_wstring(static_cast<int>(readResult.Status()));
            return std::nullopt;
        }

        auto reader = streams::DataReader::FromBuffer(readResult.Value());
        const int level = static_cast<int>(reader.ReadByte());
        if (level < 0 || level > 100) {
            error = L"Battery Level 数值超出范围: " + std::to_wstring(level);
            return std::nullopt;
        }
        return level;
    } catch (const winrt::hresult_error& ex) {
        error = L"WinRT 异常: 0x" + std::to_wstring(static_cast<unsigned int>(ex.code())) + L" " + std::wstring(ex.message().c_str());
        return std::nullopt;
    } catch (const std::exception& ex) {
        const int len = MultiByteToWideChar(CP_UTF8, 0, ex.what(), -1, nullptr, 0);
        std::wstring text(static_cast<size_t>(std::max(0, len - 1)), L'\0');
        if (len > 1) {
            MultiByteToWideChar(CP_UTF8, 0, ex.what(), -1, text.data(), len);
        }
        error = L"标准异常: " + text;
        return std::nullopt;
    } catch (...) {
        error = L"未知异常";
        return std::nullopt;
    }
}
