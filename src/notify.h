#pragma once

#include <Windows.h>
#include <string>

void ShowBalloon(HWND hwnd, const std::wstring& title, const std::wstring& message);

