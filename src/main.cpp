#include "app.h"
#include "single_instance.h"

#include <Windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCmd) {
    SingleInstance singleInstance;
    if (!singleInstance.Acquire()) {
        MessageBoxW(nullptr, L"BlueGauge 已经在运行。", L"提示", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    App app;
    return app.Run(instance, showCmd);
}
