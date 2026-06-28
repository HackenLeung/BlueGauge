#include "bluetooth/winrt_bluetooth_status.h"

#include <Windows.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cwctype>
#include <memory>
#include <mutex>
#include <regex>
#include <string>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

namespace {
namespace bluetooth = winrt::Windows::Devices::Bluetooth;
namespace enumeration = winrt::Windows::Devices::Enumeration;
namespace foundation = winrt::Windows::Foundation;

struct AsyncWaitState {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
};

template <typename TOperation>
bool WaitForCompletion(TOperation const& operation, std::chrono::milliseconds timeout) {
    auto state = std::make_shared<AsyncWaitState>();
    operation.Completed([state](auto&&, foundation::AsyncStatus) {
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
    return operation.Status() == foundation::AsyncStatus::Completed;
}

void SetError(std::wstring* error, const std::wstring& text) {
    if (error) {
        *error = text;
    }
}

std::wstring ToWide(const char* text) {
    if (!text) {
        return {};
    }
    const int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    std::wstring result(static_cast<size_t>(std::max(0, len - 1)), L'\0');
    if (len > 1) {
        MultiByteToWideChar(CP_UTF8, 0, text, -1, result.data(), len);
    }
    return result;
}

bool TryParseBluetoothAddress(const std::wstring& instanceId, const wchar_t* expectedPrefix, uint64_t& address, std::wstring* error) {
    std::wstring upper = instanceId;
    std::transform(upper.begin(), upper.end(), upper.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towupper(ch));
    });
    std::wstring prefix = expectedPrefix;
    std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towupper(ch));
    });
    if (upper.rfind(prefix, 0) != 0) {
        SetError(error, L"InstanceId 前缀不匹配: " + instanceId);
        return false;
    }

    static const std::wregex addressPattern(LR"(DEV_([0-9A-Fa-f]{12}))");
    std::wsmatch match;
    if (!std::regex_search(instanceId, match, addressPattern)) {
        SetError(error, L"未从 InstanceId 解析到蓝牙地址: " + instanceId);
        return false;
    }

    address = 0;
    for (wchar_t ch : match[1].str()) {
        address <<= 4;
        if (ch >= L'0' && ch <= L'9') {
            address += static_cast<uint64_t>(ch - L'0');
        } else {
            address += static_cast<uint64_t>(std::towupper(ch) - L'A' + 10);
        }
    }
    return true;
}

std::wstring StatusText(bluetooth::BluetoothConnectionStatus status) {
    return status == bluetooth::BluetoothConnectionStatus::Connected ? L"Connected" : L"Disconnected";
}

BluetoothConnectionKind ToConnectionKind(bluetooth::BluetoothConnectionStatus status) {
    return status == bluetooth::BluetoothConnectionStatus::Connected
        ? BluetoothConnectionKind::Connected
        : BluetoothConnectionKind::Disconnected;
}

BluetoothConnectionKind ErrorKindFromException(const winrt::hresult_error& ex, std::wstring* error) {
    SetError(error, L"WinRT 异常: 0x" + std::to_wstring(static_cast<unsigned int>(ex.code()))
        + L" " + std::wstring(ex.message().c_str()));
    return BluetoothConnectionKind::Unknown;
}
}

BluetoothConnectionKind GetClassicBluetoothConnectionStatus(const std::wstring& instanceId, std::wstring* error) {
    try {
        uint64_t address = 0;
        if (!TryParseBluetoothAddress(instanceId, L"BTHENUM\\DEV_", address, error)) {
            return BluetoothConnectionKind::Unknown;
        }

        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        constexpr auto findTimeout = std::chrono::milliseconds(2500);
        constexpr auto openTimeout = std::chrono::milliseconds(1500);

        auto findOp = enumeration::DeviceInformation::FindAllAsync(bluetooth::BluetoothDevice::GetDeviceSelector());
        if (!WaitForCompletion(findOp, findTimeout)) {
            SetError(error, L"查找经典蓝牙 DeviceInformation 超时");
            return BluetoothConnectionKind::Unknown;
        }

        auto infos = findOp.GetResults();
        for (uint32_t index = 0; index < infos.Size(); ++index) {
            try {
                auto fromIdOp = bluetooth::BluetoothDevice::FromIdAsync(infos.GetAt(index).Id());
                if (!WaitForCompletion(fromIdOp, openTimeout)) {
                    continue;
                }
                auto candidate = fromIdOp.GetResults();
                if (candidate && candidate.BluetoothAddress() == address) {
                    const auto status = candidate.ConnectionStatus();
                    SetError(error, L"DeviceInformation 匹配，ConnectionStatus=" + StatusText(status));
                    return ToConnectionKind(status);
                }
            } catch (...) {
                continue;
            }
        }

        auto addressOp = bluetooth::BluetoothDevice::FromBluetoothAddressAsync(address);
        if (!WaitForCompletion(addressOp, findTimeout)) {
            SetError(error, L"FromBluetoothAddressAsync 经典蓝牙超时");
            return BluetoothConnectionKind::Unknown;
        }
        auto device = addressOp.GetResults();
        if (!device) {
            SetError(error, L"未找到匹配经典蓝牙设备");
            return BluetoothConnectionKind::Disconnected;
        }
        const auto status = device.ConnectionStatus();
        SetError(error, L"FromBluetoothAddressAsync 匹配，ConnectionStatus=" + StatusText(status));
        return ToConnectionKind(status);
    } catch (const winrt::hresult_error& ex) {
        return ErrorKindFromException(ex, error);
    } catch (const std::exception& ex) {
        SetError(error, L"标准异常: " + ToWide(ex.what()));
        return BluetoothConnectionKind::Unknown;
    } catch (...) {
        SetError(error, L"未知异常");
        return BluetoothConnectionKind::Unknown;
    }
}

BluetoothConnectionKind GetBleBluetoothConnectionStatus(const std::wstring& instanceId, std::wstring* error) {
    try {
        uint64_t address = 0;
        if (!TryParseBluetoothAddress(instanceId, L"BTHLE\\DEV_", address, error)) {
            return BluetoothConnectionKind::Unknown;
        }

        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        constexpr auto findTimeout = std::chrono::milliseconds(2500);
        constexpr auto openTimeout = std::chrono::milliseconds(1500);

        auto findOp = enumeration::DeviceInformation::FindAllAsync(bluetooth::BluetoothLEDevice::GetDeviceSelector());
        if (!WaitForCompletion(findOp, findTimeout)) {
            SetError(error, L"查找 BLE DeviceInformation 超时");
            return BluetoothConnectionKind::Unknown;
        }

        auto infos = findOp.GetResults();
        for (uint32_t index = 0; index < infos.Size(); ++index) {
            try {
                auto fromIdOp = bluetooth::BluetoothLEDevice::FromIdAsync(infos.GetAt(index).Id());
                if (!WaitForCompletion(fromIdOp, openTimeout)) {
                    continue;
                }
                auto candidate = fromIdOp.GetResults();
                if (candidate && candidate.BluetoothAddress() == address) {
                    const auto status = candidate.ConnectionStatus();
                    SetError(error, L"DeviceInformation 匹配，ConnectionStatus=" + StatusText(status));
                    return ToConnectionKind(status);
                }
            } catch (...) {
                continue;
            }
        }

        auto publicOp = bluetooth::BluetoothLEDevice::FromBluetoothAddressAsync(address, bluetooth::BluetoothAddressType::Public);
        if (WaitForCompletion(publicOp, findTimeout)) {
            auto device = publicOp.GetResults();
            if (device) {
                const auto status = device.ConnectionStatus();
                SetError(error, L"FromBluetoothAddressAsync(Public) 匹配，ConnectionStatus=" + StatusText(status));
                return ToConnectionKind(status);
            }
        }

        auto randomOp = bluetooth::BluetoothLEDevice::FromBluetoothAddressAsync(address, bluetooth::BluetoothAddressType::Random);
        if (!WaitForCompletion(randomOp, findTimeout)) {
            SetError(error, L"FromBluetoothAddressAsync(Random) BLE 超时");
            return BluetoothConnectionKind::Unknown;
        }
        auto device = randomOp.GetResults();
        if (!device) {
            SetError(error, L"未找到匹配 BLE 设备");
            return BluetoothConnectionKind::Disconnected;
        }
        const auto status = device.ConnectionStatus();
        SetError(error, L"FromBluetoothAddressAsync(Random) 匹配，ConnectionStatus=" + StatusText(status));
        return ToConnectionKind(status);
    } catch (const winrt::hresult_error& ex) {
        return ErrorKindFromException(ex, error);
    } catch (const std::exception& ex) {
        SetError(error, L"标准异常: " + ToWide(ex.what()));
        return BluetoothConnectionKind::Unknown;
    } catch (...) {
        SetError(error, L"未知异常");
        return BluetoothConnectionKind::Unknown;
    }
}
