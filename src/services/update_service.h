#pragma once

#include <Windows.h>
#include <atomic>
#include <string>
#include <thread>

enum UpdateStatus {
    UpdateIdle = 0,
    UpdateChecking = 1,
    UpdateLatest = 2,
    UpdateAvailable = 3,
    UpdateNoRelease = 4,
    UpdateFailed = 5
};

struct UpdateResult {
    UpdateStatus status = UpdateFailed;
    std::wstring latestVersion;
    std::wstring htmlUrl;
    std::wstring message;
};

class UpdateService {
public:
    bool CheckAsync(HWND target);
    void CompleteCheck();
    void Shutdown();

private:
    std::atomic_bool checking_ = false;
    std::thread updateThread_;
};
