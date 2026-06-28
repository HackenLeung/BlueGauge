#include "startup.h"

#include "logger.h"

#include <Windows.h>
#include <string>

namespace {
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"BlueGauge";
}

bool IsStartupEnabled() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        return false;
    }
    DWORD type = 0;
    DWORD bytes = 0;
    const LONG rc = RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &bytes);
    RegCloseKey(key);
    return rc == ERROR_SUCCESS && type == REG_SZ && bytes > 0;
}

bool SetStartupEnabled(bool enabled) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        Logger::Instance().Error(L"无法打开开机启动注册表项");
        return false;
    }

    bool ok = false;
    if (enabled) {
        wchar_t path[MAX_PATH]{};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        std::wstring value = L"\"";
        value += path;
        value += L"\"";
        ok = RegSetValueExW(key, kValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                 static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else {
        const LONG rc = RegDeleteValueW(key, kValueName);
        ok = rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND;
    }
    RegCloseKey(key);
    Logger::Instance().Info(enabled ? L"设置开机自启" : L"取消开机自启");
    return ok;
}
