#include "build_info.h"

#include "version.h"

#include <Windows.h>
#include <vector>

namespace {
std::wstring FromAnsi(const char* text) {
    if (!text || text[0] == '\0') {
        return {};
    }
    const int size = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (size <= 1) {
        return {};
    }
    std::wstring result(static_cast<size_t>(size - 1), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text, -1, result.data(), size);
    return result;
}
}

std::wstring GetBuildConfig() {
#if defined(_DEBUG)
    return L"Debug";
#elif defined(NDEBUG)
    return L"Release";
#else
    return L"Development";
#endif
}

std::wstring GetBuildTimestamp() {
    return FromAnsi(__DATE__ " " __TIME__);
}

std::wstring GetExecutablePath() {
    std::vector<wchar_t> buffer(MAX_PATH);
    DWORD length = 0;
    for (;;) {
        length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size() - 1) {
            return std::wstring(buffer.data(), length);
        }
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring GetBuildSummary() {
    return std::wstring(kAppName) + L" " + kAppVersion + L" (" + GetBuildConfig()
        + L", " + GetBuildTimestamp() + L")";
}
