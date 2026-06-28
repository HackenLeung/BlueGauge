#include "logger.h"

#include <ShlObj.h>
#include <Windows.h>
#include <KnownFolders.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace {
constexpr unsigned long long kMaxLogBytes = 1024ull * 1024ull;

std::wstring NowText() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buffer;
}

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) {
        return {};
    }
    std::string result(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, result.data(), size, nullptr, nullptr);
    return result;
}
}

Logger& Logger::Instance() {
    static Logger logger;
    return logger;
}

void Logger::Init() {
    std::lock_guard<std::mutex> lock(mutex_);
    PWSTR appData = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
        std::filesystem::path dir = std::filesystem::path(appData) / L"BlueGauge" / L"logs";
        CoTaskMemFree(appData);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        logPath_ = (dir / L"app.log").wstring();
    }
}

void Logger::Info(const std::wstring& message) {
    Write(L"INFO", message);
}

void Logger::Warn(const std::wstring& message) {
    Write(L"WARN", message);
}

void Logger::Error(const std::wstring& message) {
    Write(L"ERROR", message);
}

void Logger::Write(const wchar_t* level, const std::wstring& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (logPath_.empty()) {
        return;
    }
    RotateIfNeeded();
    std::ofstream file(logPath_, std::ios::app | std::ios::binary);
    if (!file) {
        return;
    }
    file << ToUtf8(NowText()) << " [" << ToUtf8(level) << "] " << ToUtf8(message) << "\r\n";
}

void Logger::RotateIfNeeded() {
    std::error_code ec;
    if (logPath_.empty() || !std::filesystem::exists(logPath_, ec)) {
        return;
    }
    if (std::filesystem::file_size(logPath_, ec) <= kMaxLogBytes) {
        return;
    }
    std::filesystem::path path(logPath_);
    std::filesystem::path old = path;
    old += L".old";
    std::filesystem::remove(old, ec);
    std::filesystem::rename(path, old, ec);
}
