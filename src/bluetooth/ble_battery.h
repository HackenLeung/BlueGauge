#pragma once

#include <optional>
#include <string>

std::optional<int> TryReadBleBatteryLevel(const std::wstring& instanceId, std::wstring& error);

