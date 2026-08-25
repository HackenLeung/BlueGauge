#include "bluetooth/hidpp_battery.h"

#include "logger.h"

#include <Windows.h>
#include <SetupAPI.h>
// hidsdi.h 内部会 include hidpi.h，且必须由它先定义 NTSTATUS，不能反过来。
#include <hidsdi.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <iterator>
#include <memory>
#include <optional>
#include <string>

namespace {
// HID++ 长报文：reportId 0x11，收发均为 20 字节。
// 实测短报文集合（reportId 0x10）只回 HID++ 1.0 的 0x8F 错误，不可用。
constexpr USAGE kHidppUsagePage = 0xFF00;
constexpr BYTE kLongReportId = 0x11;
constexpr USHORT kLongReportSize = 20;

constexpr USHORT kVendorLogitech = 0x046D;

// SwID 取非 0，用于把主动通知（SwID 恒为 0）和我们的应答区分开。
constexpr BYTE kSwId = 0x05;

constexpr BYTE kFeatureRoot = 0x00;
constexpr USHORT kFeatureDeviceName = 0x0005;
constexpr USHORT kFeatureBatteryStatus = 0x1000;
constexpr USHORT kFeatureUnifiedBattery = 0x1004;

constexpr BYTE kMaxDeviceIndex = 6;
constexpr int kPingTimeoutMs = 250;
constexpr int kRequestTimeoutMs = 400;

struct DevInfoHandleDeleter {
    void operator()(HDEVINFO handle) const {
        if (handle != INVALID_HANDLE_VALUE) {
            SetupDiDestroyDeviceInfoList(handle);
        }
    }
};
using DevInfoHandle = std::unique_ptr<std::remove_pointer_t<HDEVINFO>, DevInfoHandleDeleter>;

struct HandleDeleter {
    void operator()(HANDLE handle) const {
        if (handle && handle != INVALID_HANDLE_VALUE) {
            CloseHandle(handle);
        }
    }
};
using ScopedHandle = std::unique_ptr<std::remove_pointer_t<HANDLE>, HandleDeleter>;

struct PreparsedDeleter {
    void operator()(PHIDP_PREPARSED_DATA data) const {
        if (data) {
            HidD_FreePreparsedData(data);
        }
    }
};
using ScopedPreparsed = std::unique_ptr<std::remove_pointer_t<PHIDP_PREPARSED_DATA>, PreparsedDeleter>;

struct ReceiverInterface {
    std::wstring path;
    USHORT productId = 0;
};

std::wstring Hex2(BYTE value) {
    wchar_t buffer[8]{};
    swprintf_s(buffer, L"%02X", value);
    return buffer;
}

// 接口路径形如 \\?\hid#vid_046d&pid_c53f&mi_02&col03#7&2720658e&0&0002#{guid}
// 第二个 # 后是实例段 7&2720658e&0&0002，其中第二个字段（2720658e）是父设备实例
// 哈希：同一物理接收器的各 HID 集合共享它，不同接收器必然不同。
//
// 只能取这一个字段，不能哈希整个实例段——末尾的 &0&0002 是集合序号，跟着集合变。
// 那样一旦枚举到的集合换了，ID 就变，置顶和别名会全部失配。
std::wstring ReceiverInstanceTag(const std::wstring& path) {
    const size_t first = path.find(L'#');
    const size_t second = first == std::wstring::npos ? std::wstring::npos : path.find(L'#', first + 1);
    if (second == std::wstring::npos) {
        return L"0000";
    }
    const size_t third = path.find(L'#', second + 1);
    const std::wstring instance = path.substr(second + 1,
        third == std::wstring::npos ? std::wstring::npos : third - second - 1);

    // 取 & 分隔的第二个字段。
    const size_t begin = instance.find(L'&');
    if (begin == std::wstring::npos) {
        return L"0000";
    }
    const size_t end = instance.find(L'&', begin + 1);
    const std::wstring parent = instance.substr(begin + 1,
        end == std::wstring::npos ? std::wstring::npos : end - begin - 1);
    if (parent.empty()) {
        return L"0000";
    }

    // FNV-1a，只为把变长实例串折成固定宽度，不需要密码学强度。
    uint32_t hash = 2166136261u;
    for (wchar_t ch : parent) {
        hash ^= static_cast<uint32_t>(towlower(ch));
        hash *= 16777619u;
    }
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"%08X", hash);
    return buffer;
}

// 带超时的重叠写，避免设备无应答时卡死扫描线程。
bool WriteWithTimeout(HANDLE device, const BYTE* data, size_t size, int timeoutMs) {
    ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) {
        return false;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();

    DWORD written = 0;
    if (WriteFile(device, data, static_cast<DWORD>(size), &written, &overlapped)) {
        return true;
    }
    if (GetLastError() != ERROR_IO_PENDING) {
        return false;
    }
    if (WaitForSingleObject(event.get(), static_cast<DWORD>(timeoutMs)) != WAIT_OBJECT_0) {
        // CancelIoEx 只请求取消，不等它完成。必须用 bWait=TRUE 等 IO 真正收尾，
        // 否则函数返回后驱动可能还在往已销毁的 overlapped / 已关闭的事件上回写。
        CancelIoEx(device, &overlapped);
        GetOverlappedResult(device, &overlapped, &written, TRUE);
        return false;
    }
    return GetOverlappedResult(device, &overlapped, &written, FALSE) != FALSE;
}

// 带超时的重叠读。返回实际读到的字节数，0 表示超时或失败。
size_t ReadWithTimeout(HANDLE device, BYTE* buffer, size_t size, int timeoutMs) {
    ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) {
        return 0;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();

    DWORD read = 0;
    if (!ReadFile(device, buffer, static_cast<DWORD>(size), &read, &overlapped)) {
        if (GetLastError() != ERROR_IO_PENDING) {
            return 0;
        }
        if (WaitForSingleObject(event.get(), static_cast<DWORD>(timeoutMs)) != WAIT_OBJECT_0) {
            // 同上：等取消真正完成，避免 overlapped / event 提前销毁。
            CancelIoEx(device, &overlapped);
            GetOverlappedResult(device, &overlapped, &read, TRUE);
            return 0;
        }
        if (!GetOverlappedResult(device, &overlapped, &read, FALSE)) {
            return 0;
        }
    }
    return static_cast<size_t>(read);
}

using LongReport = std::array<BYTE, kLongReportSize>;

// 发一条 HID++ 请求并等待匹配的应答。
// 接收器会混入鼠标移动、连接通知等无关报文，必须按 (deviceIndex, featureIndex, funcSwid) 过滤。
bool Request(HANDLE device, BYTE deviceIndex, BYTE featureIndex, BYTE funcSwid,
             const BYTE* params, size_t paramCount, LongReport& response, int timeoutMs) {
    LongReport request{};
    request[0] = static_cast<BYTE>(kLongReportId);
    request[1] = deviceIndex;
    request[2] = featureIndex;
    request[3] = funcSwid;
    for (size_t i = 0; i < paramCount && 4 + i < request.size(); ++i) {
        request[4 + i] = params[i];
    }

    if (!WriteWithTimeout(device, request.data(), request.size(), timeoutMs)) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) {
            break;
        }

        LongReport incoming{};
        const size_t read = ReadWithTimeout(device, incoming.data(), incoming.size(),
                                           static_cast<int>(remaining));
        if (read < 4) {
            continue;
        }
        if (incoming[0] != static_cast<BYTE>(kLongReportId) || incoming[1] != deviceIndex) {
            continue;
        }
        // 0x8F = HID++ 1.0 错误，0xFF = HID++ 2.0 错误，都说明这条请求不被支持。
        if (incoming[2] == 0x8F || incoming[2] == 0xFF) {
            return false;
        }
        if (incoming[2] == featureIndex && incoming[3] == funcSwid) {
            response = incoming;
            return true;
        }
    }
    return false;
}

// Root.GetFeature：查某个 feature 在该设备上的索引。0 表示不支持。
// feature 索引是每台设备各自分配的，不能写死。
BYTE GetFeatureIndex(HANDLE device, BYTE deviceIndex, USHORT feature) {
    const BYTE params[2]{
        static_cast<BYTE>(feature >> 8),
        static_cast<BYTE>(feature & 0xFF)
    };
    LongReport response{};
    if (!Request(device, deviceIndex, kFeatureRoot, static_cast<BYTE>(0x00 | kSwId),
                 params, std::size(params), response, kRequestTimeoutMs)) {
        return 0;
    }
    return response[4];
}

// Root.GetProtocolVersion，用来判断该索引上是否真的有设备在线。
bool Ping(HANDLE device, BYTE deviceIndex, int& major, int& minor) {
    const BYTE params[3]{ 0x00, 0x00, 0xAA };
    LongReport response{};
    if (!Request(device, deviceIndex, kFeatureRoot, static_cast<BYTE>(0x10 | kSwId),
                 params, std::size(params), response, kPingTimeoutMs)) {
        return false;
    }
    major = response[4];
    minor = response[5];
    return true;
}

// 0x1000 BatteryStatus：返回离散档位（如 90 / 50 / 20 / 5），不是连续百分比。
std::optional<int> ReadBatteryStatus(HANDLE device, BYTE deviceIndex) {
    const BYTE featureIndex = GetFeatureIndex(device, deviceIndex, kFeatureBatteryStatus);
    if (featureIndex == 0) {
        return std::nullopt;
    }
    LongReport response{};
    if (!Request(device, deviceIndex, featureIndex, static_cast<BYTE>(0x00 | kSwId),
                 nullptr, 0, response, kRequestTimeoutMs)) {
        return std::nullopt;
    }
    const int level = response[4];
    if (level < 0 || level > 100) {
        return std::nullopt;
    }
    return level;
}

// 0x1004 UnifiedBattery：Bolt 时代的新设备常只支持这个。
std::optional<int> ReadUnifiedBattery(HANDLE device, BYTE deviceIndex) {
    const BYTE featureIndex = GetFeatureIndex(device, deviceIndex, kFeatureUnifiedBattery);
    if (featureIndex == 0) {
        return std::nullopt;
    }
    LongReport response{};
    if (!Request(device, deviceIndex, featureIndex, static_cast<BYTE>(0x10 | kSwId),
                 nullptr, 0, response, kRequestTimeoutMs)) {
        return std::nullopt;
    }

    const int stateOfCharge = response[4];
    if (stateOfCharge >= 1 && stateOfCharge <= 100) {
        return stateOfCharge;
    }

    // 不报百分比的设备只给档位标志位，折算成一个近似值。
    const BYTE levelFlags = response[5];
    if (levelFlags & 0x08) {
        return 100;  // full
    }
    if (levelFlags & 0x04) {
        return 70;   // good
    }
    if (levelFlags & 0x02) {
        return 20;   // low
    }
    if (levelFlags & 0x01) {
        return 5;    // critical
    }
    return std::nullopt;
}

// 0x0005 DeviceNameAndType：先取长度，再按 16 字节一段读回来。
std::wstring ReadDeviceName(HANDLE device, BYTE deviceIndex) {
    const BYTE featureIndex = GetFeatureIndex(device, deviceIndex, kFeatureDeviceName);
    if (featureIndex == 0) {
        return {};
    }

    LongReport countResponse{};
    if (!Request(device, deviceIndex, featureIndex, static_cast<BYTE>(0x00 | kSwId),
                 nullptr, 0, countResponse, kRequestTimeoutMs)) {
        return {};
    }
    const int total = countResponse[4];
    if (total <= 0) {
        return {};
    }

    std::string name;
    for (int offset = 0; offset < total && offset < 128; offset += 16) {
        const BYTE params[1]{ static_cast<BYTE>(offset) };
        LongReport chunk{};
        if (!Request(device, deviceIndex, featureIndex, static_cast<BYTE>(0x10 | kSwId),
                     params, std::size(params), chunk, kRequestTimeoutMs)) {
            break;
        }
        for (size_t i = 4; i < chunk.size() && static_cast<int>(name.size()) < total; ++i) {
            if (chunk[i] == 0) {
                break;
            }
            name.push_back(static_cast<char>(chunk[i]));
        }
    }

    while (!name.empty() && (name.back() == ' ' || name.back() == '\0')) {
        name.pop_back();
    }
    if (name.empty()) {
        return {};
    }
    return std::wstring(name.begin(), name.end());
}

// 找出所有罗技接收器的 HID++ 长报文集合。
std::vector<ReceiverInterface> FindLogitechReceivers() {
    std::vector<ReceiverInterface> receivers;

    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);

    DevInfoHandle set(SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
                                           DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (set.get() == INVALID_HANDLE_VALUE) {
        Logger::Instance().Warn(L"2.4G 扫描: 枚举 HID 接口失败，Win32 错误: "
            + std::to_wstring(GetLastError()));
        return receivers;
    }

    for (DWORD index = 0;; ++index) {
        SP_DEVICE_INTERFACE_DATA interfaceData{};
        interfaceData.cbSize = sizeof(interfaceData);
        if (!SetupDiEnumDeviceInterfaces(set.get(), nullptr, &hidGuid, index, &interfaceData)) {
            break;
        }

        DWORD required = 0;
        SetupDiGetDeviceInterfaceDetailW(set.get(), &interfaceData, nullptr, 0, &required, nullptr);
        if (required == 0) {
            continue;
        }

        std::vector<BYTE> buffer(required);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buffer.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set.get(), &interfaceData, detail, required,
                                              &required, nullptr)) {
            continue;
        }

        const std::wstring path = detail->DevicePath;

        // 用 0 访问权限打开：即使集合被独占，也能拿到属性和描述符。
        ScopedHandle probe(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, 0, nullptr));
        if (!probe || probe.get() == INVALID_HANDLE_VALUE) {
            continue;
        }

        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        if (!HidD_GetAttributes(probe.get(), &attributes)
            || attributes.VendorID != kVendorLogitech) {
            continue;
        }

        PHIDP_PREPARSED_DATA rawPreparsed = nullptr;
        if (!HidD_GetPreparsedData(probe.get(), &rawPreparsed)) {
            continue;
        }
        ScopedPreparsed preparsed(rawPreparsed);

        HIDP_CAPS caps{};
        if (HidP_GetCaps(preparsed.get(), &caps) != HIDP_STATUS_SUCCESS) {
            continue;
        }
        if (caps.UsagePage != kHidppUsagePage
            || caps.InputReportByteLength != kLongReportSize
            || caps.OutputReportByteLength != kLongReportSize) {
            continue;
        }

        receivers.push_back({ path, attributes.ProductID });
        Logger::Instance().Info(L"2.4G 扫描: 找到罗技 HID++ 长报文接口 PID_"
            + Hex2(static_cast<BYTE>(attributes.ProductID >> 8))
            + Hex2(static_cast<BYTE>(attributes.ProductID & 0xFF)));
    }

    return receivers;
}
}  // namespace

std::vector<BluetoothDeviceInfo> ScanLogitechHidppDevices() {
    std::vector<BluetoothDeviceInfo> devices;

    for (const auto& receiver : FindLogitechReceivers()) {
        ScopedHandle handle(CreateFileW(receiver.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (!handle || handle.get() == INVALID_HANDLE_VALUE) {
            Logger::Instance().Warn(L"2.4G 扫描: 打开接收器失败，Win32 错误: "
                + std::to_wstring(GetLastError()));
            continue;
        }

        for (BYTE deviceIndex = 1; deviceIndex <= kMaxDeviceIndex; ++deviceIndex) {
            int major = 0;
            int minor = 0;
            if (!Ping(handle.get(), deviceIndex, major, minor)) {
                continue;  // 未配对或已休眠
            }

            BluetoothDeviceInfo device;
            // 带上接收器实例标识：同型号的两个接收器 PID 相同、设备索引都从 1 起，
            // 只用 VID+PID+DEV 会撞 ID，导致置顶 / 别名 / 低电量去重 / 连接状态互相串台。
            wchar_t idBuffer[96]{};
            swprintf_s(idBuffer, L"HIDPP\\VID_%04X&PID_%04X&RCV_%s&DEV_%02X",
                       kVendorLogitech, receiver.productId,
                       ReceiverInstanceTag(receiver.path).c_str(), deviceIndex);
            device.id = idBuffer;
            device.connected = true;
            device.source = L"2.4G";

            device.name = ReadDeviceName(handle.get(), deviceIndex);
            if (device.name.empty()) {
                device.name = L"罗技 2.4G 设备 " + Hex2(deviceIndex);
            }

            device.batteryPercent = ReadBatteryStatus(handle.get(), deviceIndex);
            if (!device.batteryPercent.has_value()) {
                device.batteryPercent = ReadUnifiedBattery(handle.get(), deviceIndex);
            }

            if (device.batteryPercent.has_value()) {
                Logger::Instance().Info(device.name + L" 2.4G 电量读取成功: "
                    + std::to_wstring(*device.batteryPercent) + L"% (HID++ "
                    + std::to_wstring(major) + L"." + std::to_wstring(minor) + L")");
            } else {
                Logger::Instance().Warn(device.name
                    + L" 2.4G 在线但未读到电量，不支持 0x1000/0x1004");
            }

            devices.push_back(std::move(device));
        }
    }

    return devices;
}
