#pragma once

#include <Windows.h>

class SingleInstance {
public:
    bool Acquire();
    ~SingleInstance();

private:
    HANDLE mutex_ = nullptr;
};

