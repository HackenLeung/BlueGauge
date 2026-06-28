#include "services/bluetooth_device_watcher.h"

#include "logger.h"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <exception>
#include <string>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/base.h>

namespace {
namespace bluetooth = winrt::Windows::Devices::Bluetooth;
namespace enumeration = winrt::Windows::Devices::Enumeration;
namespace foundation = winrt::Windows::Foundation;

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

std::wstring HResultText(winrt::hresult code) {
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(code));
    return buffer;
}

struct WatcherRegistration {
    enumeration::DeviceWatcher watcher{ nullptr };
    winrt::event_token addedToken{};
    winrt::event_token removedToken{};
    winrt::event_token updatedToken{};
    winrt::event_token completedToken{};
    winrt::event_token stoppedToken{};
    bool added = false;
    bool removed = false;
    bool updated = false;
    bool completed = false;
    bool stopped = false;
};
}

struct BluetoothDeviceWatcherService::Impl {
    bool Start(HWND hwnd, UINT msg);
    void Stop();

private:
    void StartWatcher(WatcherRegistration& state, std::atomic_bool& enumerated, winrt::hstring const& selector, const wchar_t* source);
    void StopWatcher(WatcherRegistration& state);
    void NotifyDeviceChanged(const std::wstring& source, const wchar_t* eventName, winrt::hstring const& id, bool shouldPost);
    void NotifyWatcherState(const std::wstring& source, const wchar_t* eventName);

    std::atomic<HWND> target{ nullptr };
    std::atomic<UINT> message{ 0 };
    std::atomic_bool running{ false };
    std::atomic_bool classicEnumerated{ false };
    std::atomic_bool bleEnumerated{ false };
    WatcherRegistration classic;
    WatcherRegistration ble;
};

BluetoothDeviceWatcherService::BluetoothDeviceWatcherService()
    : impl_(std::make_unique<Impl>()) {
}

BluetoothDeviceWatcherService::~BluetoothDeviceWatcherService() {
    Stop();
}

bool BluetoothDeviceWatcherService::Start(HWND target, UINT message) {
    return impl_->Start(target, message);
}

void BluetoothDeviceWatcherService::Stop() {
    impl_->Stop();
}

bool BluetoothDeviceWatcherService::Impl::Start(HWND hwnd, UINT msg) {
    if (!hwnd || msg == 0) {
        return false;
    }
    bool expected = false;
    if (!running.compare_exchange_strong(expected, true)) {
        return true;
    }

    target.store(hwnd);
    message.store(msg);

    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        classicEnumerated.store(false);
        bleEnumerated.store(false);
        StartWatcher(classic, classicEnumerated, bluetooth::BluetoothDevice::GetDeviceSelector(), L"Classic");
        StartWatcher(ble, bleEnumerated, bluetooth::BluetoothLEDevice::GetDeviceSelector(), L"BLE");
        Logger::Instance().Info(L"蓝牙设备变化监听已启动");
        return true;
    } catch (const winrt::hresult_error& ex) {
        Logger::Instance().Warn(L"蓝牙 DeviceWatcher 启动失败: " + HResultText(ex.code()) + L" " + std::wstring(ex.message().c_str()));
    } catch (const std::exception& ex) {
        Logger::Instance().Warn(L"蓝牙 DeviceWatcher 启动失败: " + ToWide(ex.what()));
    } catch (...) {
        Logger::Instance().Warn(L"蓝牙 DeviceWatcher 启动失败: 未知异常");
    }

    Stop();
    return false;
}

void BluetoothDeviceWatcherService::Impl::Stop() {
    running.store(false);
    target.store(nullptr);
    message.store(0);
    classicEnumerated.store(false);
    bleEnumerated.store(false);
    StopWatcher(classic);
    StopWatcher(ble);
}

void BluetoothDeviceWatcherService::Impl::StartWatcher(WatcherRegistration& state, std::atomic_bool& enumerated, winrt::hstring const& selector, const wchar_t* source) {
    state.watcher = enumeration::DeviceInformation::CreateWatcher(selector);
    const std::wstring sourceText = source;

    state.addedToken = state.watcher.Added([this, sourceText, &enumerated](enumeration::DeviceWatcher const&, enumeration::DeviceInformation const& info) {
        NotifyDeviceChanged(sourceText, L"Added", info.Id(), enumerated.load());
    });
    state.added = true;

    state.removedToken = state.watcher.Removed([this, sourceText, &enumerated](enumeration::DeviceWatcher const&, enumeration::DeviceInformationUpdate const& info) {
        NotifyDeviceChanged(sourceText, L"Removed", info.Id(), enumerated.load());
    });
    state.removed = true;

    state.updatedToken = state.watcher.Updated([this, sourceText, &enumerated](enumeration::DeviceWatcher const&, enumeration::DeviceInformationUpdate const& info) {
        NotifyDeviceChanged(sourceText, L"Updated", info.Id(), enumerated.load());
    });
    state.updated = true;

    state.completedToken = state.watcher.EnumerationCompleted([this, sourceText, &enumerated](enumeration::DeviceWatcher const&, foundation::IInspectable const&) {
        enumerated.store(true);
        NotifyWatcherState(sourceText, L"EnumerationCompleted");
    });
    state.completed = true;

    state.stoppedToken = state.watcher.Stopped([this, sourceText](enumeration::DeviceWatcher const&, foundation::IInspectable const&) {
        NotifyWatcherState(sourceText, L"Stopped");
    });
    state.stopped = true;

    state.watcher.Start();
}

void BluetoothDeviceWatcherService::Impl::StopWatcher(WatcherRegistration& state) {
    if (!state.watcher) {
        return;
    }

    try {
        if (state.added) {
            state.watcher.Added(state.addedToken);
        }
        if (state.removed) {
            state.watcher.Removed(state.removedToken);
        }
        if (state.updated) {
            state.watcher.Updated(state.updatedToken);
        }
        if (state.completed) {
            state.watcher.EnumerationCompleted(state.completedToken);
        }
        if (state.stopped) {
            state.watcher.Stopped(state.stoppedToken);
        }

        const auto status = state.watcher.Status();
        if (status == enumeration::DeviceWatcherStatus::Started
            || status == enumeration::DeviceWatcherStatus::EnumerationCompleted) {
            state.watcher.Stop();
        }
    } catch (const winrt::hresult_error& ex) {
        Logger::Instance().Warn(L"蓝牙 DeviceWatcher 停止失败: " + HResultText(ex.code()) + L" " + std::wstring(ex.message().c_str()));
    } catch (...) {
        Logger::Instance().Warn(L"蓝牙 DeviceWatcher 停止失败: 未知异常");
    }

    state = WatcherRegistration{};
}

void BluetoothDeviceWatcherService::Impl::NotifyDeviceChanged(const std::wstring& source, const wchar_t* eventName, winrt::hstring const& id, bool shouldPost) {
    Logger::Instance().Info(L"蓝牙设备变化事件: " + source + L" " + eventName
        + (shouldPost ? L" " : L" 初始枚举 ")
        + std::wstring(id.c_str()));
    if (!shouldPost) {
        return;
    }
    const HWND hwnd = target.load();
    const UINT msg = message.load();
    if (running.load() && hwnd && msg != 0 && IsWindow(hwnd)) {
        PostMessageW(hwnd, msg, 0, 0);
    }
}

void BluetoothDeviceWatcherService::Impl::NotifyWatcherState(const std::wstring& source, const wchar_t* eventName) {
    Logger::Instance().Info(L"蓝牙设备监听状态: " + source + L" " + eventName);
}
