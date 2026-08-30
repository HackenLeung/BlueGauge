#include "bluetooth/xctech_battery.h"

#include "logger.h"

#include <Windows.h>
#include <SetupAPI.h>
// hidsdi.h 内部会 include hidpi.h，且必须由它先定义 NTSTATUS，不能反过来。
#include <hidsdi.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cwctype>
#include <memory>
#include <optional>
#include <string>

namespace {
// XCTECH 私有协议：接收器除了鼠标 / 键盘 / 消费控制之外，还会多暴露一个
// 私有 HID 集合（实测在 MI_02 上，但下面按用法和报文长度识别，不认接口序号），
// 收发均 33 字节。首字节是 report id 0x00，后面 32 字节才是 payload。
//
// 协议由厂商 App（Inphic Mouse 1.0.2.6）反汇编得到，只用其中只读的一条命令：
//   请求  payload[0] = 0x10 (GetDeviceInfo)，其余为 0
//   应答  payload[0] = 0x10 回显，payload[1] = 0x00 表示成功
// 两个方向的校验和都放在 payload[31]，覆盖 payload[4..30]。
constexpr USHORT kReportSize = 33;
constexpr size_t kPayloadSize = kReportSize - 1;

constexpr BYTE kReportId = 0x00;
constexpr BYTE kOpGetDeviceInfo = 0x10;
constexpr BYTE kStatusOk = 0x00;

// payload 下标。和厂商 App 里的 65 字节 device info 缓存一一对应。
constexpr size_t kOffsetOpcode = 0;
constexpr size_t kOffsetStatus = 1;
constexpr size_t kOffsetDevId = 4;       // 4 字节 ASCII，如 "M909"
constexpr size_t kDevIdLength = 4;
constexpr size_t kOffsetFirmware = 8;    // u16 little-endian，如 0x0184 = 1.32
constexpr size_t kOffsetCharging = 12;
constexpr size_t kOffsetBattery = 13;
constexpr size_t kOffsetValid = 14;      // 0 表示这一帧里的鼠标数据无效
constexpr size_t kChecksumBegin = 4;
constexpr size_t kChecksumEnd = 30;      // 含
constexpr size_t kOffsetChecksum = 31;

constexpr int kRequestTimeoutMs = 400;

// 厂商 config.xml 里声明的接收器。0x248A 是有线，0x249A 是 2.4G 接收器。
// 同一只鼠标插线和用接收器会走不同的 VID/PID，因此两种都要认。
struct KnownInterface {
    USHORT vendorId;
    USHORT productId;
    const wchar_t* source;
};
constexpr KnownInterface kKnownInterfaces[]{
    { 0x249A, 0x5C2F, L"2.4G" },
    { 0x248A, 0x5C2F, L"2.4G" },
    { 0x248A, 0x5C2E, L"USB" },
    { 0x248A, 0x5D2E, L"USB" },
    { 0x248A, 0x5E2E, L"USB" },
};

// dev_id -> 展示名，取自厂商 config.xml。表里没有的型号退回 dev_id 本身，
// 这样新机型至少能显示出来，用户也可以在 BlueGauge 里重命名。
struct KnownModel {
    const char* devId;
    const wchar_t* name;
};
constexpr KnownModel kKnownModels[]{
    { "M909", L"IN9 Gaming Mouse" },
    { "IN90", L"IN9 Gaming Mouse" },
    { "M910", L"IN9 Gaming Mouse" },
    { "M900", L"A9" },
};

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
    USHORT vendorId = 0;
    USHORT productId = 0;
    const wchar_t* source = L"2.4G";
};

std::wstring Hex4(USHORT value) {
    wchar_t buffer[8]{};
    swprintf_s(buffer, L"%04X", value);
    return buffer;
}

// 接口路径形如 \\?\hid#vid_249a&pid_5c2f&mi_02#8&23e31b92&0&0000#{guid}
// 第二个 # 后是实例段 8&23e31b92&0&0000，其中第二个字段（23e31b92）是父设备
// 实例哈希：同一物理接收器的各 HID 集合共享它，不同接收器必然不同。
//
// 只能取这一个字段，不能哈希整个实例段——末尾的 &0&0000 是集合序号，跟着集合变。
// 那样一旦枚举到的集合换了，ID 就变，置顶和别名会全部失配。
// 与 hidpp_battery.cpp 里的同名函数保持一致的取法。
std::wstring ReceiverInstanceTag(const std::wstring& path) {
    const size_t first = path.find(L'#');
    const size_t second = first == std::wstring::npos ? std::wstring::npos : path.find(L'#', first + 1);
    if (second == std::wstring::npos) {
        return L"0000";
    }
    const size_t third = path.find(L'#', second + 1);
    const std::wstring instance = path.substr(second + 1,
        third == std::wstring::npos ? std::wstring::npos : third - second - 1);

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
//
// failed 用来区分「等满了超时」和「立刻就失败」（比如扫描途中拔掉了接收器）。
// 两者都返回 0，但后者不消耗时间：调用方若一律重试，会在剩余时间里空转。
size_t ReadWithTimeout(HANDLE device, BYTE* buffer, size_t size, int timeoutMs,
                       bool& failed) {
    failed = false;
    ScopedHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) {
        failed = true;
        return 0;
    }

    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();

    DWORD read = 0;
    if (!ReadFile(device, buffer, static_cast<DWORD>(size), &read, &overlapped)) {
        if (GetLastError() != ERROR_IO_PENDING) {
            failed = true;
            return 0;
        }
        if (WaitForSingleObject(event.get(), static_cast<DWORD>(timeoutMs)) != WAIT_OBJECT_0) {
            // 同上：等取消真正完成，避免 overlapped / event 提前销毁。
            CancelIoEx(device, &overlapped);
            GetOverlappedResult(device, &overlapped, &read, TRUE);
            return 0;
        }
        if (!GetOverlappedResult(device, &overlapped, &read, FALSE)) {
            failed = true;
            return 0;
        }
    }
    return static_cast<size_t>(read);
}

using Report = std::array<BYTE, kReportSize>;
using Payload = std::array<BYTE, kPayloadSize>;

BYTE Checksum(const Payload& payload) {
    unsigned sum = 0;
    for (size_t i = kChecksumBegin; i <= kChecksumEnd; ++i) {
        sum += payload[i];
    }
    return static_cast<BYTE>(sum & 0xFF);
}

// 发 GetDeviceInfo 并等回显。
// 厂商程序也在读同一个集合时，可能混入别的命令的应答，所以要按 opcode 过滤，
// 再用校验和确认这一帧没被截断。
bool RequestDeviceInfo(HANDLE device, Payload& response) {
    Report request{};
    request[0] = kReportId;
    Payload outgoing{};
    outgoing[kOffsetOpcode] = kOpGetDeviceInfo;
    outgoing[kOffsetChecksum] = Checksum(outgoing);
    for (size_t i = 0; i < kPayloadSize; ++i) {
        request[1 + i] = outgoing[i];
    }

    if (!WriteWithTimeout(device, request.data(), request.size(), kRequestTimeoutMs)) {
        return false;
    }

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(kRequestTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) {
            break;
        }

        Report incoming{};
        bool failed = false;
        const size_t read = ReadWithTimeout(device, incoming.data(), incoming.size(),
                                           static_cast<int>(remaining), failed);
        if (failed) {
            return false;  // 句柄已经不能用了，继续重试只是空转
        }
        if (read < kReportSize) {
            continue;
        }

        Payload candidate{};
        for (size_t i = 0; i < kPayloadSize; ++i) {
            candidate[i] = incoming[1 + i];
        }
        if (candidate[kOffsetOpcode] != kOpGetDeviceInfo
            || candidate[kOffsetStatus] != kStatusOk) {
            continue;  // 鼠标移动上报、别的命令的应答，或设备回了错误
        }
        if (candidate[kOffsetChecksum] != Checksum(candidate)) {
            continue;
        }
        response = candidate;
        return true;
    }
    return false;
}

std::string ReadDevId(const Payload& payload) {
    std::string devId;
    for (size_t i = 0; i < kDevIdLength; ++i) {
        const BYTE ch = payload[kOffsetDevId + i];
        if (ch < 0x20 || ch > 0x7E) {
            break;
        }
        devId.push_back(static_cast<char>(ch));
    }
    return devId;
}

std::wstring ModelName(const std::string& devId) {
    for (const auto& model : kKnownModels) {
        if (devId == model.devId) {
            return model.name;
        }
    }
    if (devId.empty()) {
        return {};
    }
    return std::wstring(devId.begin(), devId.end());
}

// 找出所有 XCTECH 接收器的私有 HID 集合。
// 判定依据和厂商程序一致：VID/PID 命中已知表，且集合是 UsagePage 1 / Usage 0
// 的 33 字节收发对。Usage 0 本身不是合法的 HID 用法，正好把它和真正的
// 鼠标 / 键盘 / 消费控制集合区分开。
std::vector<ReceiverInterface> FindXctechInterfaces() {
    std::vector<ReceiverInterface> interfaces;

    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);

    DevInfoHandle set(SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
                                           DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (set.get() == INVALID_HANDLE_VALUE) {
        Logger::Instance().Warn(L"英菲克扫描: 枚举 HID 接口失败，Win32 错误: "
            + std::to_wstring(GetLastError()));
        return interfaces;
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
        if (!HidD_GetAttributes(probe.get(), &attributes)) {
            continue;
        }

        const wchar_t* source = nullptr;
        for (const auto& known : kKnownInterfaces) {
            if (attributes.VendorID == known.vendorId
                && attributes.ProductID == known.productId) {
                source = known.source;
                break;
            }
        }
        if (!source) {
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
        if (caps.InputReportByteLength != kReportSize
            || caps.OutputReportByteLength != kReportSize) {
            continue;
        }
        // 同一个接收器上鼠标 / 键盘 / 消费控制集合的收发长度都不是 33 字节，
        // 单靠长度已经能区分。这里再按用法收一道，避免以后某个机型多出一个
        // 同样 33 字节的集合时，我们对着错误的集合发命令。
        const bool usageMatches =
            (caps.UsagePage == 0x0001 && caps.Usage == 0x0000)
            || (caps.UsagePage == 0xFF01 && caps.Usage == 0x0010);
        if (!usageMatches) {
            continue;
        }

        interfaces.push_back({ path, attributes.VendorID, attributes.ProductID, source });
        Logger::Instance().Info(L"英菲克扫描: 找到私有 HID 接口 VID_"
            + Hex4(attributes.VendorID) + L"&PID_" + Hex4(attributes.ProductID));
    }

    return interfaces;
}
}  // namespace

std::vector<BluetoothDeviceInfo> ScanXctechDevices() {
    std::vector<BluetoothDeviceInfo> devices;

    for (const auto& receiver : FindXctechInterfaces()) {
        // 带上接收器实例标识：同型号的两个接收器 VID/PID 相同，
        // 只用 VID+PID 会撞 ID，导致置顶 / 别名 / 低电量去重 / 连接状态互相串台。
        wchar_t idBuffer[96]{};
        swprintf_s(idBuffer, L"XCTECH\\VID_%04X&PID_%04X&RCV_%s",
                   receiver.vendorId, receiver.productId,
                   ReceiverInstanceTag(receiver.path).c_str());
        const std::wstring id = idBuffer;

        // 同一个接收器万一暴露出两个都符合特征的集合，两条会算出同一个 ID。
        // 放任重复会让列表出现两个同 ID 条目，置顶和别名跟着错乱，所以先去重。
        const bool duplicate = std::any_of(devices.begin(), devices.end(),
            [&id](const BluetoothDeviceInfo& existing) { return existing.id == id; });
        if (duplicate) {
            continue;
        }

        ScopedHandle handle(CreateFileW(receiver.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (!handle || handle.get() == INVALID_HANDLE_VALUE) {
            Logger::Instance().Warn(L"英菲克扫描: 打开接收器失败，Win32 错误: "
                + std::to_wstring(GetLastError()));
            continue;
        }

        Payload payload{};
        if (!RequestDeviceInfo(handle.get(), payload)) {
            // 接收器在但鼠标关机 / 休眠 / 未配对时不会应答，按不在线处理。
            Logger::Instance().Info(L"英菲克扫描: VID_" + Hex4(receiver.vendorId)
                + L"&PID_" + Hex4(receiver.productId) + L" 无应答，设备可能未开机");
            continue;
        }

        BluetoothDeviceInfo device;
        device.id = id;
        device.connected = true;
        device.source = receiver.source;

        const std::string devId = ReadDevId(payload);
        device.name = ModelName(devId);
        if (device.name.empty()) {
            // 机型码读不出来时兜底，按实际连接方式拼名字，别写死 2.4G。
            device.name = std::wstring(L"英菲克 ") + receiver.source + L" 设备";
        }

        const unsigned firmware = static_cast<unsigned>(payload[kOffsetFirmware])
            | (static_cast<unsigned>(payload[kOffsetFirmware + 1]) << 8);
        const bool charging = payload[kOffsetCharging] != 0;

        // payload[14] 为 0 时这一帧的鼠标数据无效，厂商程序同样只在非 0 时刷新 UI。
        const int level = payload[kOffsetBattery];
        if (payload[kOffsetValid] != 0 && level >= 1 && level <= 100) {
            device.batteryPercent = level;
            Logger::Instance().Info(device.name + L" " + receiver.source + L" 电量读取成功: "
                + std::to_wstring(level) + L"%"
                + (charging ? L"（充电中）" : L"")
                + L" (固件 0x" + Hex4(static_cast<USHORT>(firmware)) + L")");
        } else {
            Logger::Instance().Warn(device.name + L" " + receiver.source
                + L" 在线但未读到有效电量");
        }

        devices.push_back(std::move(device));
    }

    return devices;
}
