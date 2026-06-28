#include "bluetooth/btc_battery.h"

#include "logger.h"

#include <Windows.h>
#include <SetupAPI.h>
#include <cfgmgr32.h>
#include <devguid.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <memory>
#include <regex>
#include <set>
#include <vector>

namespace {
struct DevInfoHandleDeleter {
    void operator()(HDEVINFO handle) const {
        if (handle != INVALID_HANDLE_VALUE) {
            SetupDiDestroyDeviceInfoList(handle);
        }
    }
};
using DevInfoHandle = std::unique_ptr<std::remove_pointer_t<HDEVINFO>, DevInfoHandleDeleter>;

struct CandidateNode {
    std::wstring instanceId;
    std::wstring className;
    std::wstring friendlyName;
    DEVINST devInst = 0;
    int priority = 0;
};

std::wstring Upper(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towupper(ch));
    });
    return text;
}

std::wstring GetRegistryString(HDEVINFO set, SP_DEVINFO_DATA& data, DWORD property) {
    wchar_t buffer[512]{};
    DWORD type = 0;
    DWORD size = 0;
    if (SetupDiGetDeviceRegistryPropertyW(set, &data, property, &type, reinterpret_cast<PBYTE>(buffer), sizeof(buffer), &size)) {
        return buffer;
    }
    return {};
}

std::wstring DescribeCandidate(const CandidateNode& node) {
    std::wstring text = node.instanceId;
    if (!node.className.empty()) {
        text += L", Class=" + node.className;
    }
    if (!node.friendlyName.empty()) {
        text += L", Name=" + node.friendlyName;
    }
    return text;
}

int CandidatePriority(const std::wstring& instanceId, const std::wstring& className, bool systemPass) {
    const std::wstring upperId = Upper(instanceId);
    const std::wstring upperClass = Upper(className);
    if ((systemPass || upperClass == L"SYSTEM") && upperId.find(L"0000111E") != std::wstring::npos) {
        return 0;
    }
    if (systemPass || upperClass == L"SYSTEM") {
        return 1;
    }
    if (upperId.find(L"0000111E") != std::wstring::npos) {
        return 2;
    }
    return 3;
}

void CollectCandidates(HDEVINFO set, const std::wstring& mac, bool systemPass, std::set<std::wstring>& seen, std::vector<CandidateNode>& candidates) {
    for (DWORD index = 0;; ++index) {
        SP_DEVINFO_DATA data{};
        data.cbSize = sizeof(data);
        if (!SetupDiEnumDeviceInfo(set, index, &data)) {
            break;
        }

        wchar_t instanceBuffer[512]{};
        if (!SetupDiGetDeviceInstanceIdW(set, &data, instanceBuffer, static_cast<DWORD>(std::size(instanceBuffer)), nullptr)) {
            continue;
        }

        std::wstring instanceId = instanceBuffer;
        const std::wstring upperId = Upper(instanceId);
        if (upperId.find(L"BTHENUM\\") == std::wstring::npos || upperId.find(mac) == std::wstring::npos) {
            continue;
        }
        if (!seen.insert(upperId).second) {
            continue;
        }

        std::wstring friendlyName = GetRegistryString(set, data, SPDRP_FRIENDLYNAME);
        if (friendlyName.empty()) {
            friendlyName = GetRegistryString(set, data, SPDRP_DEVICEDESC);
        }
        std::wstring className = GetRegistryString(set, data, SPDRP_CLASS);
        candidates.push_back({
            instanceId,
            className,
            friendlyName,
            data.DevInst,
            CandidatePriority(instanceId, className, systemPass)
        });
    }
}

CONFIGRET ReadBatteryProperty(DEVINST devInst, const DEVPROPKEY& key, std::optional<int>& battery) {
    DEVPROPTYPE propType = 0;
    std::array<BYTE, 16> buffer{};
    ULONG propSize = static_cast<ULONG>(buffer.size());
    CONFIGRET rc = CM_Get_DevNode_PropertyW(devInst, &key, &propType, buffer.data(), &propSize, 0);
    if (rc != CR_SUCCESS) {
        return rc;
    }

    int value = -1;
    if (propSize >= sizeof(BYTE)) {
        value = static_cast<int>(buffer[0]);
    }
    if (propSize >= sizeof(DWORD) && (propType == DEVPROP_TYPE_UINT32 || propType == DEVPROP_TYPE_INT32)) {
        DWORD dwordValue = 0;
        std::memcpy(&dwordValue, buffer.data(), sizeof(dwordValue));
        value = static_cast<int>(dwordValue);
    }
    if (value >= 0 && value <= 100) {
        battery = value;
    }
    return rc;
}
}

std::optional<int> TryReadClassicBluetoothBattery(const std::wstring& instanceId, std::wstring& error) {
    static DEVPROPKEY bluetoothBatteryKey{
        { 0x104ea319, 0x6ee2, 0x4701, { 0xbd, 0x47, 0x8d, 0xdb, 0xf4, 0x25, 0xbb, 0xe5 } },
        2
    };

    std::wsmatch match;
    static const std::wregex macPattern(LR"(DEV_([0-9A-Fa-f]{12}))");
    if (!std::regex_search(instanceId, match, macPattern)) {
        error = L"未从经典蓝牙 InstanceId 解析到 MAC";
        return std::nullopt;
    }
    const std::wstring mac = Upper(match[1].str());

    std::set<std::wstring> seen;
    std::vector<CandidateNode> candidates;

    DevInfoHandle systemSet(SetupDiGetClassDevsW(&GUID_DEVCLASS_SYSTEM, nullptr, nullptr, DIGCF_PRESENT));
    if (systemSet.get() != INVALID_HANDLE_VALUE) {
        CollectCandidates(systemSet.get(), mac, true, seen, candidates);
    } else {
        Logger::Instance().Warn(L"经典蓝牙电量: 枚举 System 类失败，Win32 错误: " + std::to_wstring(GetLastError()));
    }

    DevInfoHandle allSet(SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES));
    if (allSet.get() == INVALID_HANDLE_VALUE) {
        error = L"SetupDiGetClassDevsW(DIGCF_ALLCLASSES) 失败: " + std::to_wstring(GetLastError());
        return std::nullopt;
    }
    CollectCandidates(allSet.get(), mac, false, seen, candidates);

    std::sort(candidates.begin(), candidates.end(), [](const CandidateNode& a, const CandidateNode& b) {
        if (a.priority != b.priority) {
            return a.priority < b.priority;
        }
        return a.instanceId < b.instanceId;
    });

    CONFIGRET lastRc = CR_NO_SUCH_VALUE;
    for (const auto& candidate : candidates) {
        std::optional<int> battery;
        CONFIGRET rc = ReadBatteryProperty(candidate.devInst, bluetoothBatteryKey, battery);
        lastRc = rc;
        Logger::Instance().Info(L"经典蓝牙电量候选: " + DescribeCandidate(candidate)
            + L", CM 返回: " + std::to_wstring(rc));
        if (battery.has_value()) {
            Logger::Instance().Info(L"经典蓝牙电量读取成功: " + DescribeCandidate(candidate)
                + L", 电量: " + std::to_wstring(*battery) + L"%");
            return battery;
        }

        DEVINST devNode = 0;
        rc = CM_Locate_DevNodeW(&devNode, const_cast<DEVINSTID_W>(candidate.instanceId.c_str()), CM_LOCATE_DEVNODE_NORMAL);
        if (rc == CR_SUCCESS) {
            battery.reset();
            rc = ReadBatteryProperty(devNode, bluetoothBatteryKey, battery);
            lastRc = rc;
            Logger::Instance().Info(L"经典蓝牙电量候选 Locate: " + DescribeCandidate(candidate)
                + L", CM 返回: " + std::to_wstring(rc));
            if (battery.has_value()) {
                Logger::Instance().Info(L"经典蓝牙电量读取成功: " + DescribeCandidate(candidate)
                    + L", 电量: " + std::to_wstring(*battery) + L"%");
                return battery;
            }
        } else {
            lastRc = rc;
            Logger::Instance().Info(L"经典蓝牙电量候选 Locate: " + DescribeCandidate(candidate)
                + L", CM 返回: " + std::to_wstring(rc));
        }
    }

    error = L"已枚举 " + std::to_wstring(candidates.size())
        + L" 个匹配节点但均未读取到电量，最后 CM 返回: " + std::to_wstring(lastRc);
    return std::nullopt;
}
