#pragma once

#include <mutex>
#include <string>

class Logger {
public:
    static Logger& Instance();

    void Init();
    void Info(const std::wstring& message);
    void Warn(const std::wstring& message);
    void Error(const std::wstring& message);

private:
    Logger() = default;
    void Write(const wchar_t* level, const std::wstring& message);
    void RotateIfNeeded();

    std::mutex mutex_;
    std::wstring logPath_;
};

