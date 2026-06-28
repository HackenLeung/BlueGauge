#pragma once

#include <optional>
#include <string>

std::optional<int> TryReadClassicBluetoothBattery(const std::wstring& instanceId, std::wstring& error);

