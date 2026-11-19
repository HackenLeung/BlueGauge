#include "bluetooth/mchose_battery.h"

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
#include <string>
#include <vector>

namespace {
// MCHOSE（迈从）前后两代接收器用的是两套完全不同的私有协议，都只能只读查询。
//
// 键盘（41E4:2001，G87 等）：20 字节报文集，输出 report 0x13。
//   请求  13 4A 01 00 ... 00 <chk>     chk = 全部字节求和，放在最后一字节
//   应答  13 4A 01 00 02 31 01 00 ...  <chk>
//         [5] 电量，[6] 低 4 位 fullStatus、高 4 位 chargeStatus
//
// 鼠标（3837:xxxx）：64 字节报文集，用输出 report 0x4D。
//   请求  4D 01 01 00 00 09 00 00 <chk>        chk = XOR(frame[2..7]) = 0x08
//         [2]=0x01 固定标志，[3]=数据长度（0 表示纯查询），
//         [4..5]=命令 0x0900（小端存放），[6]=业务号，[7]=序号
//   应答  4D 01 01 <len> 00 09 00 00 <data...> <chk>
//         chk = XOR(frame[2 .. 7+len])，紧跟在数据之后
//         data[0..1]=VID(小端)，data[2..3]=设备 PID，data[4..7]=固件
//         data[10]=连接状态，data[11]=充电状态，data[12]=电量，data[13]=游戏模式
//
// 两套协议都来自厂商 M HUB 网驱的 WebHID 实现，社区已有真机验证；
// 鼠标这条帧是用 DevTools 钩 sendReport 从官方页面实录下来的。
constexpr USHORT kMouseVendorId = 0x3837;
constexpr USHORT kKeyboardVendorId = 0x41E4;

constexpr size_t kKeyboardReportSize = 20;
constexpr BYTE kKeyboardReportId = 0x13;
constexpr BYTE kKeyboardCommand = 0x4A;
constexpr size_t kKeyboardBatteryOffset = 5;
constexpr size_t kKeyboardStatusOffset = 6;

constexpr size_t kMouseReportSize = 64;
constexpr BYTE kMouseReportId = 0x4D;
constexpr BYTE kMouseGetDeviceInfo = 0x09;
// 下面三个是回包「数据区」内的下标，数据区从帧的 [8] 开始。
constexpr size_t kMouseDataOffset = 8;
constexpr size_t kMouseConnectOffset = 10;
constexpr size_t kMouseChargeOffset = 11;
constexpr size_t kMouseBatteryOffset = 12;

// 接收器在设备休眠时不会应答。单次等待太长会拖慢整轮扫描，太短又会误判成掉线。
constexpr int kRequestTimeoutMs = 320;
constexpr int kKeyboardAttempts = 3;
constexpr int kMouseAttempts = 4;

enum class ChannelKind {
    Keyboard,
    Mouse,
};

struct Channel {
    std::wstring path;
    USHORT vendorId = 0;
    USHORT productId = 0;
    ChannelKind kind = ChannelKind::Keyboard;
    std::wstring product;
};

struct Reading {
    bool answered = false;   // 收到了合法回包（哪怕电量读不出来）
    int level = 0;
    bool charging = false;
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

std::wstring Hex4(USHORT value) {
    wchar_t buffer[8]{};
    swprintf_s(buffer, L"%04X", value);
    return buffer;
}

// 接口路径形如 \\?\hid#vid_3837&pid_1018&mi_02&col02#8&23e31b92&0&0000#{guid}
// 第二个 # 后是实例段，其中第二个字段是父设备实例哈希：同一接收器的各集合共享它。
// 取法和 hidpp_battery.cpp / xctech_battery.cpp 保持一致，这样置顶和别名不会因为
// 枚举顺序变化而失配。
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
        // 必须等取消真正完成，否则返回后驱动可能还在往已销毁的 overlapped 回写。
        CancelIoEx(device, &overlapped);
        GetOverlappedResult(device, &overlapped, &written, TRUE);
        return false;
    }
    return GetOverlappedResult(device, &overlapped, &written, FALSE) != FALSE;
}

size_t ReadWithTimeout(HANDLE device, BYTE* buffer, size_t size, int timeoutMs, bool& failed) {
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

bool SumChecksumValid(const BYTE* report, size_t size) {
    unsigned sum = 0;
    for (size_t i = 0; i + 1 < size; ++i) {
        sum += report[i];
    }
    return static_cast<BYTE>(sum & 0xFF) == report[size - 1];
}

bool XorChecksumValid(const BYTE* report, size_t size, size_t lengthIndex) {
    if (lengthIndex >= size) {
        return false;
    }
    const size_t payloadLength = report[lengthIndex];
    const size_t checksumIndex = 8 + payloadLength;
    if (checksumIndex >= size) {
        return false;
    }
    BYTE value = 0;
    for (size_t i = 2; i <= 7 + payloadLength && i < size; ++i) {
        value ^= report[i];
    }
    return value == report[checksumIndex];
}

// 键盘：输出 report 0x13，回包校验和是求和。
Reading QueryKeyboard(HANDLE device) {
    Reading reading;

    std::array<BYTE, kKeyboardReportSize> request{};
    request[0] = kKeyboardReportId;
    request[1] = kKeyboardCommand;
    request[2] = 0x01;
    BYTE sum = 0;
    for (size_t i = 0; i + 1 < request.size(); ++i) {
        sum = static_cast<BYTE>(sum + request[i]);
    }
    request[kKeyboardReportSize - 1] = sum;

    if (!WriteWithTimeout(device, request.data(), request.size(), kRequestTimeoutMs)) {
        return reading;
    }

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(kRequestTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) {
            break;
        }

        std::array<BYTE, kKeyboardReportSize> reply{};
        bool failed = false;
        const size_t read = ReadWithTimeout(device, reply.data(), reply.size(),
                                           static_cast<int>(remaining), failed);
        if (failed) {
            return reading;
        }
        if (read < kKeyboardReportSize) {
            continue;
        }
        if (reply[0] != kKeyboardReportId || reply[1] != kKeyboardCommand) {
            continue;  // 键盘自身的按键上报也走这个集，按帧头过滤
        }
        if (!SumChecksumValid(reply.data(), reply.size())) {
            continue;
        }

        reading.answered = true;
        const int raw = reply[kKeyboardBatteryOffset];
        if (raw >= 1 && raw <= 100) {
            // 固件给的是细分值，官方驱动按 5% 步进显示：实测固件报 47 时驱动显示 50。
            // 这里向上取整到 5 的倍数，和厂商界面保持一致，避免"47% vs 50%"的偏差。
            reading.level = std::min(100, ((raw + 4) / 5) * 5);
        }
        // 状态字节低 4 位是满电标志，高 4 位才是充电中；只有高 4 位可信。
        reading.charging = (reply[kKeyboardStatusOffset] >> 4) != 0;
        return reading;
    }

    return reading;
}

// 鼠标：输出 report 0x4D + 命令 0x0900（设备信息，含电量），回包校验和是异或。
Reading QueryMouse(HANDLE device) {
    Reading reading;

    std::array<BYTE, kMouseReportSize> request{};
    request[0] = kMouseReportId;
    request[1] = 0x01;
    // [2] 必须是 0x01：缺了它接收器只把请求原样回环，不当成查询。
    request[2] = 0x01;
    request[3] = 0x00;                      // 数据长度，纯查询为 0
    request[4] = 0x00;                      // 命令 0x0900，小端存放
    request[5] = kMouseGetDeviceInfo;       // 0x09
    BYTE value = 0;
    for (size_t i = 2; i <= 7; ++i) {
        value ^= request[i];
    }
    request[8] = value;

    if (!WriteWithTimeout(device, request.data(), request.size(), kRequestTimeoutMs)) {
        return reading;
    }

    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(kRequestTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) {
            break;
        }

        std::array<BYTE, kMouseReportSize> reply{};
        bool failed = false;
        const size_t read = ReadWithTimeout(device, reply.data(), reply.size(),
                                           static_cast<int>(remaining), failed);
        if (failed) {
            return reading;
        }
        if (read < kMouseReportSize) {
            continue;
        }
        if (reply[0] != kMouseReportId || reply[1] != 0x01
            || reply[4] != 0x00 || reply[5] != kMouseGetDeviceInfo) {
            continue;
        }
        if (!XorChecksumValid(reply.data(), reply.size(), 3)) {
            continue;
        }

        reading.answered = true;
        // 数据区长度不足时（回环帧就属于这种，[3] 是 0）没有电量可读。
        if (reply[3] < 13) {
            return reading;
        }
        if (reply[kMouseDataOffset + kMouseConnectOffset] == 0) {
            return reading;  // 接收器在，但设备没在转发数据
        }

        const int level = reply[kMouseDataOffset + kMouseBatteryOffset];
        if (level >= 1 && level <= 100) {
            reading.level = level;
        }
        reading.charging = reply[kMouseDataOffset + kMouseChargeOffset] != 0;
        return reading;
    }

    return reading;
}

// 找出 MCHOSE 接收器上能跑这两套协议的报文集。按收发长度识别，不认接口序号：
// 同一家的不同型号接口排布并不一致，键盘的 0x13 通道实测就落在 FF02 上，
// 而厂商设备表里标的是 FF04。
std::vector<Channel> FindChannels() {
    std::vector<Channel> channels;

    GUID hidGuid{};
    HidD_GetHidGuid(&hidGuid);

    DevInfoHandle set(SetupDiGetClassDevsW(&hidGuid, nullptr, nullptr,
                                           DIGCF_PRESENT | DIGCF_DEVICEINTERFACE));
    if (set.get() == INVALID_HANDLE_VALUE) {
        Logger::Instance().Warn(L"迈从扫描: 枚举 HID 接口失败，Win32 错误: "
            + std::to_wstring(GetLastError()));
        return channels;
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

        // 用 0 访问权限打开：即使集合被键盘 / 鼠标驱动独占，也能拿到属性和描述符。
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
        if (attributes.VendorID != kMouseVendorId && attributes.VendorID != kKeyboardVendorId) {
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
        if (caps.UsagePage < 0xFF00) {
            continue;  // 只要厂商自定义页，标准键盘 / 鼠标集合不碰
        }

        Channel channel;
        channel.path = path;
        channel.vendorId = attributes.VendorID;
        channel.productId = attributes.ProductID;

        if (caps.InputReportByteLength == kKeyboardReportSize
            && caps.OutputReportByteLength == kKeyboardReportSize) {
            channel.kind = ChannelKind::Keyboard;
        } else if (caps.InputReportByteLength == kMouseReportSize
                   && caps.OutputReportByteLength == kMouseReportSize) {
            channel.kind = ChannelKind::Mouse;
        } else {
            continue;
        }

        wchar_t product[256]{};
        if (HidD_GetProductString(probe.get(), product, sizeof(product))) {
            channel.product = product;
        }

        channels.push_back(std::move(channel));
    }

    return channels;
}

std::wstring DeviceName(const Channel& channel) {
    const std::wstring& product = channel.product;
    const bool generic = product.empty() || product == L"MCHOSE 2.4G Wireless";
    if (!generic) {
        return product;
    }
    return channel.kind == ChannelKind::Keyboard ? L"MCHOSE 键盘 (2.4G)" : L"MCHOSE 鼠标 (2.4G)";
}
}  // namespace

std::vector<BluetoothDeviceInfo> ScanMchoseDevices() {
    std::vector<BluetoothDeviceInfo> devices;

    for (const auto& channel : FindChannels()) {
        // 接收器实例 + 通道类型一起构成 ID：同一只接收器上键盘和鼠标是两条独立通道，
        // 只按 VID/PID 会撞 ID，置顶、别名和低电量去重都会串台。
        wchar_t idBuffer[128]{};
        swprintf_s(idBuffer, L"MCHOSE\\VID_%04X&PID_%04X&RCV_%s&%s",
                   channel.vendorId, channel.productId,
                   ReceiverInstanceTag(channel.path).c_str(),
                   channel.kind == ChannelKind::Keyboard ? L"KB" : L"MS");
        const std::wstring id = idBuffer;

        const bool duplicate = std::any_of(devices.begin(), devices.end(),
            [&id](const BluetoothDeviceInfo& existing) { return existing.id == id; });
        if (duplicate) {
            continue;
        }

        ScopedHandle handle(CreateFileW(channel.path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr));
        if (!handle || handle.get() == INVALID_HANDLE_VALUE) {
            Logger::Instance().Warn(L"迈从扫描: 打开接收器失败，Win32 错误: "
                + std::to_wstring(GetLastError()));
            continue;
        }

        Reading reading;
        int attempts = channel.kind == ChannelKind::Keyboard ? kKeyboardAttempts : kMouseAttempts;
        for (int attempt = 0; attempt < attempts && !reading.answered; ++attempt) {
            reading = channel.kind == ChannelKind::Keyboard
                ? QueryKeyboard(handle.get())
                : QueryMouse(handle.get());
        }

        const std::wstring name = DeviceName(channel);
        if (!reading.answered) {
            // 接收器在但设备关机 / 休眠 / 未配对时不会应答，按不在线处理。
            Logger::Instance().Info(L"迈从扫描: VID_" + Hex4(channel.vendorId)
                + L"&PID_" + Hex4(channel.productId) + L" 无应答，设备可能未开机");
            continue;
        }

        BluetoothDeviceInfo device;
        device.id = id;
        device.name = name;
        device.systemName = name;
        device.source = L"2.4G";

        if (reading.level < 1) {
            // 回包合法但没有电量：接收器在，设备本身没在转发数据。
            // 这种状态和「已断开」无法区分，按不在线处理，不往列表里塞一条空设备。
            Logger::Instance().Info(name + L" 2.4G 在线但未读到有效电量（协议未匹配或设备休眠）");
            continue;
        }

        device.connected = true;
        device.batteryPercent = reading.level;
        device.charging = reading.charging;
        Logger::Instance().Info(name + L" 2.4G 电量读取成功: "
            + std::to_wstring(reading.level) + L"%"
            + (reading.charging ? L"（充电中）" : L""));

        devices.push_back(std::move(device));
    }

    return devices;
}
