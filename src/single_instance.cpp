#include "single_instance.h"

bool SingleInstance::Acquire() {
    mutex_ = CreateMutexW(nullptr, FALSE, L"Local\\BlueGauge.SingleInstance");
    return mutex_ != nullptr && GetLastError() != ERROR_ALREADY_EXISTS;
}

SingleInstance::~SingleInstance() {
    if (mutex_) {
        CloseHandle(mutex_);
    }
}
