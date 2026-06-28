#include "app.h"

#include "build_info.h"
#include "logger.h"
#include "notify.h"
#include "resource.h"
#include "startup.h"
#include "version.h"

#include <CommCtrl.h>
#include <Dbt.h>
#include <Shellapi.h>
#include <ShObjIdl.h>
#include <Uxtheme.h>
#include <Vsstyle.h>
#include <windowsx.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <memory>
#include <sstream>

namespace {
constexpr wchar_t kWindowClass[] = L"BlueGauge.HiddenWindow";
constexpr wchar_t kSettingsClass[] = L"BlueGauge.SettingsWindow";
constexpr wchar_t kStatusClass[] = L"BlueGauge.StatusWindow";
constexpr wchar_t kTrayPanelClass[] = L"BlueGauge.TrayPanelWindow";
constexpr wchar_t kDevicePanelClass[] = L"BlueGauge.DevicePanelWindow";
constexpr wchar_t kAboutClass[] = L"BlueGauge.AboutWindow";
constexpr wchar_t kUpdateClass[] = L"BlueGauge.UpdateWindow";
constexpr wchar_t kConnectionToastClass[] = L"BlueGauge.ConnectionToastWindow";
constexpr UINT_PTR kRefreshTimer = 3001;
constexpr UINT_PTR kTrayPanelCloseTimer = 3002;
constexpr UINT_PTR kConnectionToastTimer = 3003;
constexpr UINT_PTR kBluetoothChangeDebounceTimer = 3004;
constexpr UINT kConnectionToastDurationMs = 2800;
constexpr UINT kBluetoothChangeDebounceMs = 1500;
constexpr DWORD kTaskbarDisplayGraceMs = 30000;
constexpr int kTaskbarEmptyHideScans = 3;

bool TryReadInt(HWND hwnd, int id, int& value) {
    wchar_t text[32]{};
    GetDlgItemTextW(hwnd, id, text, 32);
    if (text[0] == L'\0') {
        return false;
    }
    wchar_t* end = nullptr;
    const long parsed = wcstol(text, &end, 10);
    if (end == text || *end != L'\0') {
        return false;
    }
    value = static_cast<int>(parsed);
    return true;
}

void SetIntText(HWND hwnd, int id, int value) {
    SetDlgItemTextW(hwnd, id, std::to_wstring(value).c_str());
}

enum SettingsId {
    IDC_REFRESH = 4001,
    IDC_THRESHOLD = 4002,
};

enum SettingsSection {
    SectionScan = 0,
    SectionAlerts = 1,
    SectionDisplay = 2,
    SectionSystem = 3
};

constexpr int kTrayPanelRefresh = 0;
constexpr int kTrayPanelSettings = 1;
constexpr int kTrayPanelDevices = 2;
constexpr int kTrayPanelUpdate = 3;
constexpr int kTrayPanelAbout = 4;
constexpr int kTrayPanelExit = 5;
constexpr int kMaxPinnedDevices = 3;
constexpr int kDisplayStyleCardsTop = 160;
constexpr int kDisplayStyleCardHeight = 118;
constexpr int kDisplayStyleCardGapX = 12;
constexpr int kDisplayMaxDevicesTop = 310;
constexpr int kDisplayPinnedTop = 380;
constexpr int kDisplayPinnedRowsTop = 442;
constexpr int kDisplayRefreshButtonTop = 562;
constexpr int kSegmentedWidth = 152;
constexpr int kSegmentedHeight = 36;
constexpr int kSegmentedPadding = 3;
constexpr int kSegmentedOptionCount = 3;
constexpr int kRefreshOptions[kSegmentedOptionCount] = { 10, 20, 30 };
constexpr int kThresholdOptions[kSegmentedOptionCount] = { 10, 20, 30 };
constexpr int kScanDeviceListTop = 218;
constexpr int kScanDeviceRowHeight = 44;
constexpr int kScanDeviceRowGap = 8;
constexpr int kScanButtonTop = 152;
constexpr int kScanPrimaryButtonWidth = 96;
constexpr int kScanButtonHeight = 30;

constexpr COLORREF kSurface = RGB(255, 255, 255);
constexpr COLORREF kSidebar = RGB(248, 250, 252);
constexpr COLORREF kBorder = RGB(226, 232, 240);
constexpr COLORREF kText = RGB(15, 23, 42);
constexpr COLORREF kSubtleText = RGB(100, 116, 139);
constexpr COLORREF kBlue = RGB(37, 99, 235);
constexpr COLORREF kTrack = RGB(226, 232, 240);
constexpr COLORREF kDanger = RGB(220, 38, 38);
constexpr COLORREF kTaskbarBackground = RGB(23, 33, 51);
constexpr COLORREF kTaskbarStripFill = RGB(248, 250, 252);
constexpr COLORREF kTaskbarStripText = RGB(18, 32, 51);
constexpr COLORREF kTaskbarStripTrack = RGB(217, 227, 239);
constexpr COLORREF kTaskbarItemBorder = RGB(214, 222, 232);
constexpr COLORREF kStatusTransparentColor = RGB(1, 2, 3);
constexpr int kTaskbarListPadding = 9;
constexpr int kTaskbarItemGap = 7;

bool IsTaskbarCentered() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    const LSTATUS status = RegGetValueW(HKEY_CURRENT_USER,
        L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
        L"TaskbarAl", RRF_RT_REG_DWORD, nullptr, &value, &size);
    return status != ERROR_SUCCESS || value != 0;
}

HFONT CreateUiFont(int pointSize, int weight = FW_NORMAL) {
    HDC hdc = GetDC(nullptr);
    const int height = -MulDiv(pointSize, GetDeviceCaps(hdc, LOGPIXELSY), 72);
    ReleaseDC(nullptr, hdc);

    return CreateFontW(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
}

void FillRoundRect(HDC hdc, const RECT& rect, int radius, COLORREF color) {
    HBRUSH brush = CreateSolidBrush(color);
    HGDIOBJ oldBrush = SelectObject(hdc, brush);
    HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));
    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(hdc, oldPen);
    SelectObject(hdc, oldBrush);
    DeleteObject(brush);
}

void StrokeRoundRect(HDC hdc, const RECT& rect, int radius, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void DrawTextLine(HDC hdc, const std::wstring& text, RECT rect, UINT format, HFONT font, COLORREF color) {
    HGDIOBJ oldFont = SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, color);
    DrawTextW(hdc, text.c_str(), -1, &rect, format);
    SelectObject(hdc, oldFont);
}

void DrawSeparator(HDC hdc, int left, int top, int right) {
    HPEN pen = CreatePen(PS_SOLID, 1, kBorder);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    MoveToEx(hdc, left, top, nullptr);
    LineTo(hdc, right, top);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

void DrawInputFrame(HDC hdc, const RECT& rect) {
    FillRoundRect(hdc, rect, 6, RGB(255, 255, 255));
    StrokeRoundRect(hdc, { rect.left, rect.top, rect.right - 1, rect.bottom - 1 }, 6, RGB(203, 213, 225));
}

void DrawButtonFrame(HDC hdc, const RECT& rect, const std::wstring& text, HFONT font, bool primary = false, bool enabled = true) {
    const COLORREF fill = enabled ? (primary ? kBlue : kSurface) : RGB(241, 245, 249);
    const COLORREF border = enabled ? (primary ? kBlue : RGB(203, 213, 225)) : RGB(203, 213, 225);
    const COLORREF textColor = enabled ? (primary ? RGB(255, 255, 255) : kText) : RGB(148, 163, 184);
    FillRoundRect(hdc, rect, 8, fill);
    StrokeRoundRect(hdc, { rect.left, rect.top, rect.right - 1, rect.bottom - 1 }, 8, border);
    DrawTextLine(hdc, text, rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS, font, textColor);
}

void DrawCheckBox(HDC hdc, const RECT& rect, bool checked) {
    HTHEME theme = OpenThemeData(nullptr, L"Button");
    if (theme) {
        const int state = checked ? CBS_CHECKEDNORMAL : CBS_UNCHECKEDNORMAL;
        DrawThemeBackground(theme, hdc, BP_CHECKBOX, state, &rect, nullptr);
        CloseThemeData(theme);
        return;
    }
    RECT fallback = rect;
    DrawFrameControl(hdc, &fallback, DFC_BUTTON, DFCS_BUTTONCHECK | (checked ? DFCS_CHECKED : 0));
}

RECT SettingsCheckRect(int right, int rowTop, int rowBottom) {
    constexpr int size = 16;
    const int top = rowTop + (rowBottom - rowTop - size) / 2;
    return { right - 24, top, right - 8, top + size };
}

RECT SettingsCheckHitRect(int right, int rowTop, int rowBottom) {
    return { right - 54, rowTop, right, rowBottom };
}

std::vector<BluetoothDeviceInfo> ConnectedBatteryDevices(const std::vector<BluetoothDeviceInfo>& devices) {
    std::vector<BluetoothDeviceInfo> result;
    for (const auto& device : devices) {
        if (device.connected && device.batteryPercent.has_value()) {
            result.push_back(device);
        }
    }
    return result;
}

bool ContainsDeviceId(const std::vector<std::wstring>& ids, const std::wstring& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

std::vector<BluetoothDeviceInfo> MenuDevices(const std::vector<BluetoothDeviceInfo>& devices, bool showDisconnected) {
    std::vector<BluetoothDeviceInfo> result;
    for (const auto& device : devices) {
        if (device.connected || showDisconnected) {
            result.push_back(device);
        }
    }
    return result;
}

std::vector<BluetoothDeviceInfo> TaskbarDevices(const std::vector<BluetoothDeviceInfo>& devices, const AppConfig& config) {
    std::vector<BluetoothDeviceInfo> connected = ConnectedBatteryDevices(devices);
    std::vector<BluetoothDeviceInfo> result;
    const int limit = std::clamp(config.taskbarMaxDevices, 1, 3);

    for (const auto& pinnedId : config.pinnedDeviceIds) {
        auto it = std::find_if(connected.begin(), connected.end(), [&](const BluetoothDeviceInfo& device) {
            return device.id == pinnedId;
        });
        if (it != connected.end() && result.size() < static_cast<size_t>(limit)) {
            result.push_back(*it);
        }
    }

    std::sort(connected.begin(), connected.end(), [](const BluetoothDeviceInfo& a, const BluetoothDeviceInfo& b) {
        return a.batteryPercent.value_or(101) < b.batteryPercent.value_or(101);
    });

    for (const auto& device : connected) {
        if (result.size() >= static_cast<size_t>(limit)) {
            break;
        }
        auto exists = std::find_if(result.begin(), result.end(), [&](const BluetoothDeviceInfo& item) {
            return item.id == device.id;
        });
        if (exists == result.end()) {
            result.push_back(device);
        }
    }
    return result;
}

bool PointInRect(int x, int y, const RECT& rect) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

RECT DisplayStyleCardRect(int left, int right, int top, int index) {
    const int cardW = (right - left - kDisplayStyleCardGapX) / 2;
    const int x = left + std::clamp(index, 0, 1) * (cardW + kDisplayStyleCardGapX);
    const int y = top + kDisplayStyleCardsTop;
    return { x, y, x + cardW, y + kDisplayStyleCardHeight };
}

RECT SettingsSegmentedRect(int right, int rowTop) {
    return { right - kSegmentedWidth, rowTop, right, rowTop + kSegmentedHeight };
}

RECT SettingsSegmentButtonRect(const RECT& segmented, int index) {
    const int innerLeft = segmented.left + kSegmentedPadding;
    const int innerTop = segmented.top + kSegmentedPadding;
    const int buttonW = (segmented.right - segmented.left - kSegmentedPadding * 2) / kSegmentedOptionCount;
    return {
        innerLeft + index * buttonW,
        innerTop,
        innerLeft + (index + 1) * buttonW,
        segmented.bottom - kSegmentedPadding
    };
}

int HitSettingsSegment(const RECT& segmented, int x, int y) {
    if (!PointInRect(x, y, segmented)) {
        return -1;
    }
    for (int i = 0; i < kSegmentedOptionCount; ++i) {
        if (PointInRect(x, y, SettingsSegmentButtonRect(segmented, i))) {
            return i;
        }
    }
    return -1;
}

void DrawSegmentedControl(HDC hdc, const RECT& rect, const int* values, const wchar_t* unit, int selectedValue, HFONT font) {
    FillRoundRect(hdc, rect, 7, RGB(248, 250, 252));
    StrokeRoundRect(hdc, { rect.left, rect.top, rect.right - 1, rect.bottom - 1 }, 7, RGB(226, 232, 240));
    for (int i = 0; i < kSegmentedOptionCount; ++i) {
        RECT button = SettingsSegmentButtonRect(rect, i);
        const bool selected = values[i] == selectedValue;
        if (selected) {
            FillRoundRect(hdc, button, 5, RGB(255, 255, 255));
            StrokeRoundRect(hdc, { button.left, button.top, button.right - 1, button.bottom - 1 }, 5, RGB(226, 232, 240));
        }
        const std::wstring label = std::to_wstring(values[i]) + unit;
        DrawTextLine(hdc, label, button, DT_SINGLELINE | DT_CENTER | DT_VCENTER,
            font, selected ? kBlue : RGB(71, 85, 105));
    }
}

RECT ScanDeviceRowRect(int left, int right, int top, int index) {
    const int y = top + kScanDeviceListTop + index * (kScanDeviceRowHeight + kScanDeviceRowGap);
    return { left, y, right, y + kScanDeviceRowHeight };
}

RECT ScanPrimaryButtonRect(int right, int top) {
    return { right - kScanPrimaryButtonWidth, top + kScanButtonTop, right, top + kScanButtonTop + kScanButtonHeight };
}

struct DeviceBadgeStyle {
    std::wstring text;
    COLORREF textColor = kSubtleText;
    COLORREF background = RGB(248, 250, 252);
    COLORREF border = kBorder;
    bool framed = true;
};

DeviceBadgeStyle DeviceBadge(const BluetoothDeviceInfo& device, int threshold) {
    if (!device.connected) {
        return { L"未连接", RGB(100, 116, 139), RGB(248, 250, 252), RGB(226, 232, 240) };
    }
    if (!device.batteryPercent.has_value()) {
        return { L"电量未知", RGB(100, 116, 139), RGB(255, 255, 255), RGB(255, 255, 255), false };
    }
    if (*device.batteryPercent <= threshold) {
        return { L"低电量", RGB(220, 38, 38), RGB(254, 242, 242), RGB(254, 202, 202) };
    }
    return { L"正常", RGB(22, 163, 74), RGB(255, 255, 255), RGB(255, 255, 255), false };
}

std::wstring DeviceStatusText(const BluetoothDeviceInfo& device) {
    std::wstring status = device.connected ? L"已连接" : L"未连接";
    status += L" · ";
    if (device.connected && device.batteryPercent.has_value()) {
        status += std::to_wstring(*device.batteryPercent) + L"%";
    } else {
        status += L"电量未知";
    }
    return status;
}

void DrawBadge(HDC hdc, const RECT& rect, const DeviceBadgeStyle& style, HFONT font) {
    if (style.framed) {
        FillRoundRect(hdc, rect, rect.bottom - rect.top, style.background);
        StrokeRoundRect(hdc, { rect.left, rect.top, rect.right - 1, rect.bottom - 1 }, rect.bottom - rect.top, style.border);
    }
    DrawTextLine(hdc, style.text, rect, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_END_ELLIPSIS, font, style.textColor);
}

void DrawScanDeviceRow(HDC hdc, const BluetoothDeviceInfo& device, const RECT& row, int threshold, HFONT nameFont, HFONT smallFont) {
    FillRoundRect(hdc, row, 6, RGB(255, 255, 255));
    StrokeRoundRect(hdc, { row.left, row.top, row.right - 1, row.bottom - 1 }, 6, kBorder);

    const DeviceBadgeStyle badge = DeviceBadge(device, threshold);
    RECT badgeRect{ row.right - 92, row.top + 10, row.right - 10, row.bottom - 10 };
    DrawBadge(hdc, badgeRect, badge, smallFont);

    DrawTextLine(hdc, device.name, { row.left + 10, row.top + 5, badgeRect.left - 12, row.top + 24 },
        DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS, nameFont, kText);
    DrawTextLine(hdc, DeviceStatusText(device), { row.left + 10, row.top + 23, badgeRect.left - 12, row.bottom - 5 },
        DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS, smallFont,
        device.connected ? kSubtleText : RGB(100, 116, 139));
}

COLORREF BatteryColor(int battery, int threshold) {
    if (battery <= threshold) {
        return kDanger;
    }
    if (battery <= 55) {
        return RGB(202, 138, 4);
    }
    return RGB(22, 163, 74);
}

void DrawBatteryGlyph(HDC hdc, const RECT& rect, int battery, COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    const int bodyTop = rect.top + 5;
    const int bodyBottom = rect.bottom - 5;
    Rectangle(hdc, rect.left, bodyTop, rect.right - 5, bodyBottom);
    SelectObject(hdc, GetStockObject(NULL_PEN));
    HBRUSH capBrush = CreateSolidBrush(color);
    HGDIOBJ capOldBrush = SelectObject(hdc, capBrush);
    RoundRect(hdc, rect.right - 5, bodyTop + 4, rect.right - 1, bodyBottom - 4, 2, 2);
    SelectObject(hdc, capOldBrush);
    DeleteObject(capBrush);

    const int fillWidth = std::max(2, (static_cast<int>(rect.right - rect.left) - 10) * std::clamp(battery, 0, 100) / 100);
    HBRUSH fillBrush = CreateSolidBrush(color);
    HGDIOBJ fillOldBrush = SelectObject(hdc, fillBrush);
    Rectangle(hdc, rect.left + 3, bodyTop + 3, rect.left + 3 + fillWidth, bodyBottom - 3);
    SelectObject(hdc, fillOldBrush);
    DeleteObject(fillBrush);

    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

int MeasureTextWidth(HDC hdc, HFONT font, const std::wstring& text) {
    HGDIOBJ oldFont = SelectObject(hdc, font);
    SIZE size{};
    GetTextExtentPoint32W(hdc, text.c_str(), static_cast<int>(text.size()), &size);
    SelectObject(hdc, oldFont);
    return size.cx;
}

int PercentTextWidth(HDC hdc, HFONT font, const std::wstring& text) {
    return std::max(44, MeasureTextWidth(hdc, font, text) + 4);
}

void DrawMeter(HDC hdc, const RECT& rect, int battery, COLORREF color) {
    FillRoundRect(hdc, rect, rect.bottom - rect.top, kTaskbarStripTrack);
    const int clampedBattery = std::clamp(battery, 0, 100);
    const int fillWidth = std::max(0, static_cast<int>(rect.right - rect.left) * clampedBattery / 100);
    if (fillWidth > 0) {
        FillRoundRect(hdc, { rect.left, rect.top, rect.left + fillWidth, rect.bottom }, rect.bottom - rect.top, color);
    }
}

struct TaskbarDeviceRenderInfo {
    std::wstring name;
    int battery = 0;
};

int TaskbarItemMinWidth(int style) {
    return style == 0 ? 128 : 118;
}

int TaskbarItemMaxWidth(int style) {
    return style == 0 ? 205 : 210;
}

int TaskbarItemDesiredWidth(HDC hdc, HFONT font, const TaskbarDeviceRenderInfo& device, int style) {
    const std::wstring percent = std::to_wstring(device.battery) + L"%";
    const int nameWidth = MeasureTextWidth(hdc, font, device.name);
    const int percentWidth = PercentTextWidth(hdc, font, percent);
    const int raw = style == 0
        ? 8 + nameWidth + 8 + percentWidth + 8
        : 8 + nameWidth + 8 + percentWidth + 8;
    return std::clamp(raw, TaskbarItemMinWidth(style), TaskbarItemMaxWidth(style));
}

struct TaskbarRowLayout {
    std::vector<int> itemWidths;
    int totalWidth = 0;
    int itemHeight = 28;
};

TaskbarRowLayout BuildTaskbarRowLayout(HDC hdc, HFONT font, const std::vector<TaskbarDeviceRenderInfo>& devices, int style, int maxWidth) {
    style = std::clamp(style, 0, kTaskbarBatteryStyleCount - 1);
    TaskbarRowLayout layout;
    if (devices.empty()) {
        return layout;
    }

    const int count = static_cast<int>(devices.size());
    layout.itemWidths.reserve(devices.size());
    int desiredSum = 0;
    for (const auto& device : devices) {
        const int width = TaskbarItemDesiredWidth(hdc, font, device, style);
        layout.itemWidths.push_back(width);
        desiredSum += width;
    }

    const int gapTotal = kTaskbarItemGap * (count - 1);
    const int itemCapacity = std::max(1, maxWidth - kTaskbarListPadding * 2 - gapTotal);
    if (desiredSum > itemCapacity) {
        int over = desiredSum - itemCapacity;
        while (over > 0) {
            bool reduced = false;
            for (int& width : layout.itemWidths) {
                const int minWidth = TaskbarItemMinWidth(style);
                if (width <= minWidth) {
                    continue;
                }
                --width;
                --over;
                reduced = true;
                if (over == 0) {
                    break;
                }
            }
            if (!reduced) {
                break;
            }
        }
    }

    int itemSum = 0;
    for (int width : layout.itemWidths) {
        itemSum += width;
    }
    layout.totalWidth = kTaskbarListPadding * 2 + itemSum + gapTotal;
    layout.itemHeight = style == 0 ? 28 : 30;
    return layout;
}

void DrawTaskbarDeviceItem(HDC hdc, const TaskbarDeviceRenderInfo& device, int style, const RECT& rect, int threshold, HFONT font, bool previewMode) {
    (void)previewMode;
    style = std::clamp(style, 0, kTaskbarBatteryStyleCount - 1);
    const COLORREF color = BatteryColor(device.battery, threshold);
    const std::wstring percent = std::to_wstring(device.battery) + L"%";
    const int percentWidth = PercentTextWidth(hdc, font, percent);

    if (style == 0) {
        FillRoundRect(hdc, rect, 6, kTaskbarStripFill);
        StrokeRoundRect(hdc, { rect.left, rect.top, rect.right - 1, rect.bottom - 1 }, 6, kTaskbarItemBorder);
        const RECT percentRect{ rect.right - 8 - percentWidth, rect.top, rect.right - 8, rect.bottom };
        RECT nameRect{ rect.left + 8, rect.top, percentRect.left - 8, rect.bottom };
        if (nameRect.right > nameRect.left + 8) {
            DrawTextLine(hdc, device.name, nameRect,
                DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS, font, kTaskbarStripText);
        }
        DrawTextLine(hdc, percent, percentRect, DT_SINGLELINE | DT_RIGHT | DT_VCENTER, font, color);
        return;
    }

    FillRoundRect(hdc, rect, 6, kTaskbarStripFill);
    StrokeRoundRect(hdc, { rect.left, rect.top, rect.right - 1, rect.bottom - 1 }, 6, kTaskbarItemBorder);
    const RECT meterRect{ rect.left + 8, rect.bottom - 7, rect.right - 8, rect.bottom - 4 };
    DrawMeter(hdc, meterRect, device.battery, color);
    const RECT percentRect{ rect.right - 8 - percentWidth, rect.top + 3, rect.right - 8, rect.bottom - 9 };
    RECT nameRect{ rect.left + 8, rect.top + 3, percentRect.left - 8, rect.bottom - 9 };
    if (nameRect.right > nameRect.left + 8) {
        DrawTextLine(hdc, device.name, nameRect,
            DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS, font, kTaskbarStripText);
    }
    DrawTextLine(hdc, percent, percentRect, DT_SINGLELINE | DT_RIGHT | DT_VCENTER, font, color);
}

void DrawTaskbarDeviceList(HDC hdc, const std::vector<TaskbarDeviceRenderInfo>& devices, int style, const RECT& rect, int threshold, HFONT font, bool previewMode) {
    if (devices.empty()) {
        return;
    }
    style = std::clamp(style, 0, kTaskbarBatteryStyleCount - 1);
    const int count = static_cast<int>(devices.size());
    const TaskbarRowLayout layout = BuildTaskbarRowLayout(hdc, font, devices, style, std::max(1, static_cast<int>(rect.right - rect.left)));
    const int itemHeight = std::min(layout.itemHeight, std::max(20, static_cast<int>(rect.bottom - rect.top) - 6));
    const int y = rect.top + (static_cast<int>(rect.bottom - rect.top) - itemHeight) / 2;
    const int contentWidth = layout.totalWidth - kTaskbarListPadding * 2;
    const int startX = std::max(rect.left + kTaskbarListPadding, rect.right - kTaskbarListPadding - contentWidth);
    int x = startX;
    for (int i = 0; i < count; ++i) {
        const int itemWidth = layout.itemWidths[static_cast<size_t>(i)];
        DrawTaskbarDeviceItem(hdc, devices[static_cast<size_t>(i)], style, { x, y, x + itemWidth, y + itemHeight }, threshold, font, previewMode);
        x += itemWidth + kTaskbarItemGap;
    }
}

void DrawDisplayStylePreview(HDC hdc, int style, const RECT& rect, int threshold, HFONT font) {
    FillRoundRect(hdc, rect, 6, RGB(241, 245, 249));
    const std::vector<TaskbarDeviceRenderInfo> sampleDevices{
        { L"FIIL Atom", 100 },
        { L"FIIL GS", 100 },
        { L"手柄", 18 }
    };
    const int maxWidth = std::max(1, static_cast<int>(rect.right - rect.left));
    int previewCount = 1;
    for (int count = 2; count <= static_cast<int>(sampleDevices.size()); ++count) {
        int desiredWidth = kTaskbarListPadding * 2 + (count - 1) * kTaskbarItemGap;
        for (int i = 0; i < count; ++i) {
            desiredWidth += TaskbarItemDesiredWidth(hdc, font, sampleDevices[static_cast<size_t>(i)], style);
        }
        if (desiredWidth <= maxWidth) {
            previewCount = count;
        } else {
            break;
        }
    }
    std::vector<TaskbarDeviceRenderInfo> previewDevices(sampleDevices.begin(), sampleDevices.begin() + std::min(previewCount, static_cast<int>(sampleDevices.size())));
    DrawTaskbarDeviceList(hdc, previewDevices, style, rect, threshold, font, true);
}
}

int App::Run(HINSTANCE instance, int showCmd) {
    (void)showCmd;
    instance_ = instance;
    Logger::Instance().Init();
    Logger::Instance().Info(L"程序启动: " + GetBuildSummary());
    Logger::Instance().Info(L"运行路径: " + GetExecutablePath());

    SetCurrentProcessExplicitAppUserModelID(L"HackenLeung.BlueGauge");

    configStore_.Load();
    configStore_.Edit().startWithWindows = IsStartupEnabled();
    configStore_.Save();
    taskbarCreatedMessage_ = RegisterWindowMessageW(L"TaskbarCreated");

    if (!CreateHiddenWindow(instance_)) {
        return 1;
    }
    tray_.Add(hwnd_, instance_);
    SyncBatteryWindow();
    StartTimer();
    bluetoothWatcher_.Start(hwnd_, WM_BLUETOOTH_DEVICE_CHANGED);
    RefreshAsync();

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
}

bool App::CreateHiddenWindow(HINSTANCE instance) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = App::WindowProc;
    wc.hInstance = instance;
    wc.lpszClassName = kWindowClass;
    wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wc)) {
        const DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            Logger::Instance().Error(L"RegisterClassW failed: " + std::to_wstring(error));
            return false;
        }
    }

    hwnd_ = CreateWindowExW(0, kWindowClass, L"BlueGauge", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 360, 220, nullptr, nullptr, instance, this);
    if (!hwnd_) {
        Logger::Instance().Error(L"CreateWindowExW failed: " + std::to_wstring(GetLastError()));
    }
    return hwnd_ != nullptr;
}

bool App::CreateBatteryWindow() {
    taskbarWindow_ = FindTaskbarWindow();
    if (!taskbarWindow_) {
        Logger::Instance().Error(L"CreateBatteryWindow failed: taskbar not found");
        return false;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = App::StatusWindowProc;
    wc.hInstance = instance_;
    wc.lpszClassName = kStatusClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    if (!RegisterClassW(&wc)) {
        const DWORD error = GetLastError();
        if (error != ERROR_CLASS_ALREADY_EXISTS) {
            Logger::Instance().Error(L"RegisterClassW(StatusWindow) failed: " + std::to_wstring(error));
            return false;
        }
    }

    statusWindow_ = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED,
        kStatusClass,
        L"BlueGauge",
        WS_POPUP,
        0,
        0,
        320,
        34,
        nullptr,
        nullptr,
        instance_,
        this);

    if (!statusWindow_) {
        Logger::Instance().Error(L"CreateBatteryWindow failed: " + std::to_wstring(GetLastError()));
        return false;
    }
    SetLayeredWindowAttributes(statusWindow_, kStatusTransparentColor, 0, LWA_COLORKEY);

    UpdateStatusWindow();
    return true;
}

void App::SyncBatteryWindow() {
    if (TaskbarDisplayDevices().empty()) {
        if (statusWindow_) {
            DestroyWindow(statusWindow_);
            statusWindow_ = nullptr;
        }
        return;
    }
    if (!statusWindow_ || !IsWindow(statusWindow_)) {
        CreateBatteryWindow();
        return;
    }
    UpdateStatusWindow();
}

void App::StartTimer() {
    KillTimer(hwnd_, kRefreshTimer);
    SetTimer(hwnd_, kRefreshTimer, static_cast<UINT>(configStore_.Get().refreshIntervalSeconds * 1000), nullptr);
}

bool App::RefreshAsync() {
    const bool started = scannerService_.RefreshAsync(hwnd_, configStore_.Get());
    scanInProgress_ = true;
    if (settingsWindow_ && IsWindow(settingsWindow_)) {
        InvalidateRect(settingsWindow_, nullptr, TRUE);
    }
    return started;
}

void App::ScheduleBluetoothChangeRefresh(const std::wstring& source) {
    Logger::Instance().Info(source + L"，计划刷新");
    KillTimer(hwnd_, kBluetoothChangeDebounceTimer);
    SetTimer(hwnd_, kBluetoothChangeDebounceTimer, kBluetoothChangeDebounceMs, nullptr);
}

void App::RefreshFromBluetoothChange() {
    KillTimer(hwnd_, kBluetoothChangeDebounceTimer);
    Logger::Instance().Info(L"蓝牙设备变化防抖触发扫描");
    if (!RefreshAsync()) {
        Logger::Instance().Info(L"设备变化触发扫描被跳过：上一轮扫描未结束");
    }
}

void App::UpdateTaskbarDisplayCache() {
    const auto visibleDevices = TaskbarDevices(devices_, configStore_.Get());
    if (!visibleDevices.empty()) {
        taskbarDisplayCache_ = visibleDevices;
        taskbarDisplayCacheTick_ = GetTickCount();
        taskbarEmptyScanCount_ = 0;
        return;
    }

    if (taskbarDisplayCache_.empty()) {
        return;
    }

    ++taskbarEmptyScanCount_;
    const DWORD now = GetTickCount();
    if (taskbarDisplayCacheTick_ == 0
        || now - taskbarDisplayCacheTick_ > kTaskbarDisplayGraceMs
        || taskbarEmptyScanCount_ >= kTaskbarEmptyHideScans) {
        Logger::Instance().Info(L"任务栏电量显示隐藏：连续未读取到可显示设备");
        taskbarDisplayCache_.clear();
        taskbarDisplayCacheTick_ = 0;
        taskbarEmptyScanCount_ = 0;
        return;
    }

    Logger::Instance().Info(L"任务栏电量显示保留上次结果，等待下一轮扫描确认");
}

std::vector<BluetoothDeviceInfo> App::TaskbarDisplayDevices() const {
    const auto visibleDevices = TaskbarDevices(devices_, configStore_.Get());
    if (!visibleDevices.empty()) {
        return visibleDevices;
    }
    if (taskbarDisplayCache_.empty() || taskbarDisplayCacheTick_ == 0) {
        return {};
    }
    const DWORD now = GetTickCount();
    if (now - taskbarDisplayCacheTick_ > kTaskbarDisplayGraceMs) {
        return {};
    }
    return taskbarDisplayCache_;
}

void App::ClearDeviceCache() {
    devices_.clear();
    taskbarDisplayCache_.clear();
    taskbarDisplayCacheTick_ = 0;
    taskbarEmptyScanCount_ = 0;
    lowBatteryNotified_.clear();
    hasScanBaseline_ = false;
    previousConnectionStates_.clear();
    recentConnectionNotifyTicks_.clear();
    scanInProgress_ = false;
    lastScanTick_ = 0;
    lastScanDeviceCount_ = 0;
    lastScanBatteryCount_ = 0;
    UpdateTray();
    SyncBatteryWindow();
    if (settingsWindow_ && IsWindow(settingsWindow_)) {
        InvalidateRect(settingsWindow_, nullptr, TRUE);
    }
    if (devicePanelWindow_ && IsWindow(devicePanelWindow_)) {
        InvalidateRect(devicePanelWindow_, nullptr, TRUE);
    }
}

std::wstring App::BuildScanSummaryText() const {
    if (scanInProgress_) {
        return L"正在扫描蓝牙设备...";
    }
    if (lastScanTick_ == 0) {
        return L"最近一次扫描到的设备会显示在这里。";
    }
    if (lastScanDeviceCount_ <= 0) {
        return L"上次扫描：刚刚 · 未发现设备";
    }
    return L"上次扫描：刚刚 · " + std::to_wstring(lastScanDeviceCount_)
        + L" 个设备 · " + std::to_wstring(lastScanBatteryCount_) + L" 个有电量";
}

void App::ApplyScanResult(std::vector<BluetoothDeviceInfo>* result) {
    if (result) {
        TrackConnectionChanges(*result);
        devices_ = std::move(*result);
        delete result;
    }
    scannerService_.CompleteScan();
    scanInProgress_ = false;
    lastScanTick_ = GetTickCount();
    lastScanDeviceCount_ = static_cast<int>(devices_.size());
    lastScanBatteryCount_ = static_cast<int>(std::count_if(devices_.begin(), devices_.end(), [](const BluetoothDeviceInfo& device) {
        return device.batteryPercent.has_value();
    }));
    UpdateTaskbarDisplayCache();
    UpdateTray();
    SyncBatteryWindow();
    CheckLowBattery();
    if (settingsWindow_ && IsWindow(settingsWindow_)) {
        InvalidateRect(settingsWindow_, nullptr, TRUE);
    }
    if (devicePanelWindow_ && IsWindow(devicePanelWindow_)) {
        InvalidateRect(devicePanelWindow_, nullptr, TRUE);
    }
}

void App::UpdateTray() {
    TrayIconState state = TrayDisconnected;
    bool hasConnected = false;
    bool hasLow = false;
    for (const auto& device : devices_) {
        hasConnected = hasConnected || device.connected;
        hasLow = hasLow || (device.connected && device.batteryPercent.has_value()
            && *device.batteryPercent < configStore_.Get().lowBatteryThreshold);
    }
    if (hasLow) {
        state = TrayWarning;
    } else if (hasConnected) {
        state = TrayNormal;
    }
    tray_.Update(state, devices_);
}

void App::UpdateStatusWindow() {
    if (!statusWindow_) {
        return;
    }

    PositionStatusWindow();
    InvalidateRect(statusWindow_, nullptr, TRUE);
}

void App::PositionStatusWindow() {
    if (!statusWindow_) {
        return;
    }
    if (!taskbarWindow_ || !IsWindow(taskbarWindow_)) {
        taskbarWindow_ = FindTaskbarWindow();
    }
    if (!taskbarWindow_) {
        return;
    }
    const auto visibleDevices = TaskbarDisplayDevices();
    const int style = std::clamp(configStore_.Get().taskbarBatteryStyle, 0, kTaskbarBatteryStyleCount - 1);
    const int count = std::min(configStore_.Get().taskbarMaxDevices, static_cast<int>(visibleDevices.size()));
    if (count <= 0) {
        return;
    }

    RECT taskbarRect{};
    if (!GetWindowRect(taskbarWindow_, &taskbarRect)) {
        Logger::Instance().Warn(L"PositionStatusWindow failed: taskbar rect unavailable");
        return;
    }
    const int taskbarWidth = taskbarRect.right - taskbarRect.left;
    const int taskbarHeight = taskbarRect.bottom - taskbarRect.top;
    if (taskbarWidth <= 0 || taskbarHeight <= 0) {
        return;
    }
    const bool horizontal = taskbarWidth >= taskbarHeight;
    const int availableWidth = std::max(1, taskbarWidth - 8);
    std::vector<TaskbarDeviceRenderInfo> renderDevices;
    renderDevices.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        renderDevices.push_back({
            visibleDevices[static_cast<size_t>(i)].name,
            std::clamp(visibleDevices[static_cast<size_t>(i)].batteryPercent.value_or(0), 0, 100)
        });
    }
    HDC measureDc = GetDC(nullptr);
    HFONT measureFont = CreateUiFont(9);
    const TaskbarRowLayout layout = BuildTaskbarRowLayout(measureDc, measureFont, renderDevices, style, availableWidth);
    DeleteObject(measureFont);
    ReleaseDC(nullptr, measureDc);
    int width = std::min(layout.totalWidth, availableWidth);
    const int preferredHeight = style == 0 ? 34 : 38;
    const int height = horizontal
        ? std::min(preferredHeight, std::max(28, taskbarHeight - 4))
        : std::min(preferredHeight, std::max(28, taskbarHeight - 8));
    const bool centeredTaskbar = horizontal && IsTaskbarCentered();

    int x = taskbarRect.left + 8;
    int y = horizontal ? taskbarRect.top + std::max(2, (taskbarHeight - height) / 2) : taskbarRect.top + 8;
    HWND trayNotify = centeredTaskbar ? nullptr : FindTrayNotifyWindow(taskbarWindow_);
    if (centeredTaskbar) {
        x = taskbarRect.left + 8;
    } else if (trayNotify) {
        RECT trayRect{};
        if (GetWindowRect(trayNotify, &trayRect)) {
            if (horizontal) {
                x = trayRect.left - width - 8;
            } else {
                x = taskbarRect.left + std::max(2, (taskbarWidth - width) / 2);
                y = trayRect.top - height - 8;
            }
        }
    } else if (horizontal) {
        x = taskbarRect.right - width - 220;
    } else {
        x = taskbarRect.left + std::max(2, (taskbarWidth - width) / 2);
    }

    const int minX = taskbarRect.left + 4;
    const int maxX = taskbarRect.right - width - 4;
    const int minY = taskbarRect.top + 4;
    const int maxY = taskbarRect.bottom - height - 4;
    x = maxX >= minX ? std::clamp(x, minX, maxX) : taskbarRect.left;
    y = maxY >= minY ? std::clamp(y, minY, maxY) : taskbarRect.top;

    positioningStatusWindow_ = true;
    SetWindowPos(statusWindow_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    positioningStatusWindow_ = false;

    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, 8, 8);
    SetWindowRgn(statusWindow_, region, TRUE);
}

HWND App::FindTaskbarWindow() const {
    return FindWindowW(L"Shell_TrayWnd", nullptr);
}

HWND App::FindTrayNotifyWindow(HWND taskbar) const {
    if (!taskbar) {
        return nullptr;
    }
    HWND trayNotify = FindWindowExW(taskbar, nullptr, L"TrayNotifyWnd", nullptr);
    if (trayNotify) {
        return trayNotify;
    }
    HWND rebar = FindWindowExW(taskbar, nullptr, L"ReBarWindow32", nullptr);
    if (rebar) {
        return FindWindowExW(rebar, nullptr, L"TrayNotifyWnd", nullptr);
    }
    return nullptr;
}

void App::PaintStatusWindow(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rect{};
    GetClientRect(hwnd, &rect);

    HBRUSH transparentBrush = CreateSolidBrush(kStatusTransparentColor);
    FillRect(hdc, &rect, transparentBrush);
    DeleteObject(transparentBrush);

    HFONT textFont = CreateUiFont(9);
    auto visibleDevices = TaskbarDisplayDevices();
    const int style = std::clamp(configStore_.Get().taskbarBatteryStyle, 0, kTaskbarBatteryStyleCount - 1);
    const int threshold = configStore_.Get().lowBatteryThreshold;

    if (visibleDevices.empty()) {
        DeleteObject(textFont);
        EndPaint(hwnd, &ps);
        return;
    }

    const int count = std::min(configStore_.Get().taskbarMaxDevices, static_cast<int>(visibleDevices.size()));
    std::vector<TaskbarDeviceRenderInfo> renderDevices;
    renderDevices.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) {
        renderDevices.push_back({
            visibleDevices[static_cast<size_t>(i)].name,
            std::clamp(visibleDevices[static_cast<size_t>(i)].batteryPercent.value_or(0), 0, 100)
        });
    }
    DrawTaskbarDeviceList(hdc, renderDevices, style, rect, threshold, textFont, false);

    DeleteObject(textFont);
    EndPaint(hwnd, &ps);
}

void App::PaintSettingsWindow(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rect{};
    GetClientRect(hwnd, &rect);

    HBRUSH surfaceBrush = CreateSolidBrush(kSurface);
    FillRect(hdc, &rect, surfaceBrush);
    DeleteObject(surfaceBrush);

    RECT sidebar{ 0, 0, 178, rect.bottom };
    HBRUSH sidebarBrush = CreateSolidBrush(kSidebar);
    FillRect(hdc, &sidebar, sidebarBrush);
    DeleteObject(sidebarBrush);

    HPEN dividerPen = CreatePen(PS_SOLID, 1, kBorder);
    HGDIOBJ oldPen = SelectObject(hdc, dividerPen);
    MoveToEx(hdc, 178, 0, nullptr);
    LineTo(hdc, 178, rect.bottom);
    SelectObject(hdc, oldPen);
    DeleteObject(dividerPen);
    DrawSeparator(hdc, 178, 70, rect.right);

    HFONT titleFont = CreateUiFont(12, FW_SEMIBOLD);
    HFONT textFont = CreateUiFont(10);
    HFONT sectionFont = CreateUiFont(10, FW_SEMIBOLD);
    HFONT smallFont = CreateUiFont(9);

    DrawTextLine(hdc, L"设置", { 24, 22, 140, 48 }, DT_SINGLELINE | DT_LEFT | DT_VCENTER, titleFont, kText);

    const struct {
        const wchar_t* text;
        int y;
    } tabs[] = {
        { L"扫描", 88 },
        { L"提醒", 138 },
        { L"显示", 188 },
        { L"系统", 238 },
    };

    for (int i = 0; i < 4; ++i) {
        const auto& tab = tabs[i];
        const bool active = activeSettingsSection_ == i;
        if (active) {
            FillRoundRect(hdc, { 12, tab.y - 5, 16, tab.y + 27 }, 4, kBlue);
            FillRoundRect(hdc, { 24, tab.y - 7, 154, tab.y + 29 }, 8, RGB(239, 246, 255));
        }
        DrawTextLine(hdc, tab.text, { 42, tab.y, 150, tab.y + 22 },
            DT_SINGLELINE | DT_LEFT | DT_VCENTER, textFont, active ? kBlue : kText);
    }

    DrawTextLine(hdc, L"BlueGauge 设置", { 210, 22, 520, 48 },
        DT_SINGLELINE | DT_LEFT | DT_VCENTER, titleFont, kText);

    const int left = 220;
    const int right = rect.right - 38;
    const int top = 82;
    const AppConfig& cfg = configStore_.Get();

    if (activeSettingsSection_ == SectionScan) {
        DrawTextLine(hdc, L"扫描", { left, top, right, top + 28 }, DT_SINGLELINE | DT_LEFT | DT_VCENTER, sectionFont, kText);
        DrawTextLine(hdc, L"刷新间隔", { left, top + 58, 430, top + 86 }, DT_SINGLELINE | DT_LEFT | DT_VCENTER, textFont, kText);
        DrawTextLine(hdc, L"后台扫描蓝牙设备电量的时间间隔", { left, top + 86, 520, top + 112 },
            DT_SINGLELINE | DT_LEFT | DT_VCENTER, smallFont, kSubtleText);
        DrawSegmentedControl(hdc, SettingsSegmentedRect(right, top + 55), kRefreshOptions, L"s", cfg.refreshIntervalSeconds, textFont);
        DrawSeparator(hdc, left, top + 130, right);

        DrawTextLine(hdc, L"设备", { left, top + 154, 430, top + 180 }, DT_SINGLELINE | DT_LEFT | DT_VCENTER, textFont, kText);
        DrawButtonFrame(hdc, ScanPrimaryButtonRect(right, top), scanInProgress_ ? L"扫描中..." : L"立即扫描",
            smallFont, true, !scanInProgress_);
        DrawTextLine(hdc, BuildScanSummaryText(), { left, top + 180, right - 116, top + 204 },
            DT_SINGLELINE | DT_LEFT | DT_VCENTER, smallFont, kSubtleText);
        if (devices_.empty()) {
            DrawTextLine(hdc, L"暂无蓝牙设备", { left, top + 222, right, top + 252 },
                DT_SINGLELINE | DT_LEFT | DT_VCENTER, smallFont, kSubtleText);
        } else {
            const int count = std::min(5, static_cast<int>(devices_.size()));
            for (int i = 0; i < count; ++i) {
                const auto& device = devices_[static_cast<size_t>(i)];
                DrawScanDeviceRow(hdc, device, ScanDeviceRowRect(left, right, top, i), cfg.lowBatteryThreshold, textFont, smallFont);
            }
        }
    } else if (activeSettingsSection_ == SectionAlerts) {
        DrawTextLine(hdc, L"提醒", { left, top, right, top + 28 }, DT_SINGLELINE | DT_LEFT | DT_VCENTER, sectionFont, kText);
        DrawTextLine(hdc, L"低电量阈值", { left, top + 58, 430, top + 86 }, DT_SINGLELINE | DT_LEFT | DT_VCENTER, textFont, kText);
        DrawTextLine(hdc, L"低于该百分比时显示系统通知", { left, top + 86, 520, top + 112 },
            DT_SINGLELINE | DT_LEFT | DT_VCENTER, smallFont, kSubtleText);
        DrawSegmentedControl(hdc, SettingsSegmentedRect(right, top + 55), kThresholdOptions, L"%", cfg.lowBatteryThreshold, textFont);
        DrawSeparator(hdc, left, top + 130, right);
        DrawTextLine(hdc, L"低电量提醒", { left, top + 154, 430, top + 182 }, DT_SINGLELINE | DT_LEFT | DT_VCENTER, textFont, kText);
        DrawTextLine(hdc, L"设备低于阈值时发送一次系统提醒", { left, top + 182, 520, top + 208 },
            DT_SINGLELINE | DT_LEFT | DT_VCENTER, smallFont, kSubtleText);
        DrawCheckBox(hdc, SettingsCheckRect(right, top + 154, top + 210), cfg.enableLowBatteryNotify);
        DrawSeparator(hdc, left, top + 230, right);
        DrawTextLine(hdc, L"连接变化提醒", { left, top + 254, 430, top + 282 }, DT_SINGLELINE | DT_LEFT | DT_VCENTER, textFont, kText);
        DrawTextLine(hdc, L"设备连接或断开时显示右下角提示", { left, top + 282, 540, top + 308 },
            DT_SINGLELINE | DT_LEFT | DT_VCENTER, smallFont, kSubtleText);
        DrawCheckBox(hdc, SettingsCheckRect(right, top + 254, top + 310), cfg.enableConnectionNotify);
    } else if (activeSettingsSection_ == SectionDisplay) {
        DrawTextLine(hdc, L"显示", { left, top, right, top + 28 }, DT_SINGLELINE | DT_LEFT | DT_VCENTER, sectionFont, kText);
        DrawTextLine(hdc, L"显示未连接设备", { left, top + 48, 450, top + 74 }, DT_SINGLELINE | DT_LEFT | DT_VCENTER, textFont, kText);
        DrawTextLine(hdc, L"关闭后只在菜单中显示已连接设备", { left, top + 74, 540, top + 98 },
            DT_SINGLELINE | DT_LEFT | DT_VCENTER, smallFont, kSubtleText);
        DrawCheckBox(hdc, SettingsCheckRect(right, top + 48, top + 100), cfg.showDisconnectedDevices);

        DrawSeparator(hdc, left, top + 112, right);
        DrawTextLine(hdc, L"显示样式", { left, top + 130, right, top + 154 }, DT_SINGLELINE | DT_LEFT | DT_VCENTER, textFont, kText);
        const wchar_t* names[] = { L"极简内联", L"细电量条" };
        const wchar_t* notes[] = {
            L"最融入任务栏，占用少，适合长期常驻。",
            L"可读性最强，不依赖小图标。"
        };
        for (int i = 0; i < kTaskbarBatteryStyleCount; ++i) {
            RECT card = DisplayStyleCardRect(left, right, top, i);
            const bool selected = cfg.taskbarBatteryStyle == i;
            FillRoundRect(hdc, card, 7, kSurface);
            StrokeRoundRect(hdc, { card.left, card.top, card.right - 1, card.bottom - 1 }, 7, selected ? kBlue : RGB(203, 213, 225));
            if (selected) {
                StrokeRoundRect(hdc, { card.left + 2, card.top + 2, card.right - 3, card.bottom - 3 }, 6, RGB(191, 219, 254));
            }
            DrawTextLine(hdc, names[i], { card.left + 13, card.top + 9, card.right - 13, card.top + 29 },
                DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS, smallFont, kText);
            DrawTextLine(hdc, notes[i], { card.left + 13, card.top + 31, card.right - 13, card.top + 57 },
                DT_WORDBREAK | DT_LEFT | DT_TOP | DT_END_ELLIPSIS, smallFont, kSubtleText);
            RECT preview{ card.left + 13, card.bottom - 52, card.right - 13, card.bottom - 12 };
            DrawDisplayStylePreview(hdc, i, preview, cfg.lowBatteryThreshold, smallFont);
        }

        DrawTextLine(hdc, L"最多显示设备数", { left, top + kDisplayMaxDevicesTop, 450, top + kDisplayMaxDevicesTop + 26 }, DT_SINGLELINE | DT_LEFT | DT_VCENTER, textFont, kText);
        DrawTextLine(hdc, L"超过数量时优先显示置顶设备，其余按低电量补位", { left, top + kDisplayMaxDevicesTop + 26, 560, top + kDisplayMaxDevicesTop + 50 },
            DT_SINGLELINE | DT_LEFT | DT_VCENTER, smallFont, kSubtleText);
        for (int i = 1; i <= 3; ++i) {
            RECT seg{ right - 168 + (i - 1) * 54, top + kDisplayMaxDevicesTop + 6, right - 168 + (i - 1) * 54 + 50, top + kDisplayMaxDevicesTop + 38 };
            const bool selected = cfg.taskbarMaxDevices == i;
            FillRoundRect(hdc, seg, 7, selected ? kSurface : RGB(248, 250, 252));
            StrokeRoundRect(hdc, { seg.left, seg.top, seg.right - 1, seg.bottom - 1 }, 7, selected ? kBlue : RGB(203, 213, 225));
            DrawTextLine(hdc, std::to_wstring(i), seg, DT_SINGLELINE | DT_CENTER | DT_VCENTER, textFont, selected ? kBlue : kText);
        }

        DrawSeparator(hdc, left, top + kDisplayPinnedTop - 14, right);
        DrawTextLine(hdc, L"置顶设备", { left, top + kDisplayPinnedTop, 450, top + kDisplayPinnedTop + 26 }, DT_SINGLELINE | DT_LEFT | DT_VCENTER, textFont, kText);
        DrawTextLine(hdc, L"最多选择 3 个；在线时优先显示，离线时不占位置", { left, top + kDisplayPinnedTop + 24, 560, top + kDisplayPinnedTop + 48 },
            DT_SINGLELINE | DT_LEFT | DT_VCENTER, smallFont, kSubtleText);
        if (devices_.empty()) {
            DrawTextLine(hdc, L"暂无可置顶设备", { left, top + kDisplayPinnedRowsTop, right - 120, top + kDisplayPinnedRowsTop + 30 },
                DT_SINGLELINE | DT_LEFT | DT_VCENTER, smallFont, kSubtleText);
        } else {
            const int count = std::min(3, static_cast<int>(devices_.size()));
            for (int i = 0; i < count; ++i) {
                const auto& device = devices_[static_cast<size_t>(i)];
                const int y = top + kDisplayPinnedRowsTop + i * 34;
                RECT row{ left, y, right, y + 30 };
                const bool pinned = ContainsDeviceId(cfg.pinnedDeviceIds, device.id);
                FillRoundRect(hdc, row, 5, RGB(248, 250, 252));
                StrokeRoundRect(hdc, { row.left, row.top, row.right - 1, row.bottom - 1 }, 5, RGB(226, 232, 240));
                DrawCheckBox(hdc, { row.left + 8, row.top + 5, row.left + 28, row.top + 25 }, pinned);
                DrawTextLine(hdc, device.name, { row.left + 38, row.top, row.right - 10, row.bottom },
                    DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS, smallFont, kText);
            }
        }
        DrawButtonFrame(hdc, { right - 98, top + kDisplayRefreshButtonTop, right, top + kDisplayRefreshButtonTop + 32 }, L"刷新设备", smallFont, false);
    } else {
        DrawTextLine(hdc, L"系统", { left, top, right, top + 28 }, DT_SINGLELINE | DT_LEFT | DT_VCENTER, sectionFont, kText);
        DrawTextLine(hdc, L"开机自启", { left, top + 58, 430, top + 86 }, DT_SINGLELINE | DT_LEFT | DT_VCENTER, textFont, kText);
        DrawTextLine(hdc, L"登录 Windows 后自动启动 BlueGauge", { left, top + 86, 540, top + 112 },
            DT_SINGLELINE | DT_LEFT | DT_VCENTER, smallFont, kSubtleText);
        DrawCheckBox(hdc, SettingsCheckRect(right, top + 58, top + 112), cfg.startWithWindows);
    }

    DeleteObject(titleFont);
    DeleteObject(textFont);
    DeleteObject(sectionFont);
    DeleteObject(smallFont);
    EndPaint(hwnd, &ps);
}

void App::LayoutSettingsControls(HWND hwnd) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    (void)rect;

    auto move = [&](int id, int x, int y, int w, int h, bool visible) {
        HWND child = GetDlgItem(hwnd, id);
        if (!child) {
            return;
        }
        SetWindowPos(child, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        ShowWindow(child, visible ? SW_SHOW : SW_HIDE);
    };

    move(IDC_REFRESH, 486, 140, 118, 28, false);
    move(IDC_THRESHOLD, 486, 140, 78, 28, false);
}

void App::SetSettingsSection(HWND hwnd, int section) {
    activeSettingsSection_ = std::clamp(section, 0, 3);
    settingsScrollY_ = 0;
    LayoutSettingsControls(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
}

void App::ShowTrayPanel() {
    if (trayPanelWindow_ && IsWindow(trayPanelWindow_)) {
        CloseTrayPanels();
        return;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = App::TrayPanelWindowProc;
    wc.hInstance = instance_;
    wc.lpszClassName = kTrayPanelClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    RegisterClassW(&wc);

    WNDCLASSW deviceWc{};
    deviceWc.lpfnWndProc = App::DevicePanelWindowProc;
    deviceWc.hInstance = instance_;
    deviceWc.lpszClassName = kDevicePanelClass;
    deviceWc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    deviceWc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    RegisterClassW(&deviceWc);

    POINT pt{};
    GetCursorPos(&pt);
    const int width = 190;
    const int height = 218;

    RECT work{};
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    HMONITOR monitor = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo)) {
        work = monitorInfo.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    }

    constexpr int gap = 8;
    int x = pt.x - width / 2;
    int y = pt.y - height - gap;
    x = std::clamp(x, static_cast<int>(work.left) + gap, static_cast<int>(work.right) - width - gap);
    y = std::clamp(y, static_cast<int>(work.top) + gap, static_cast<int>(work.bottom) - height - gap);

    trayPanelWindow_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kTrayPanelClass,
        L"BlueGauge 菜单",
        WS_POPUP,
        x,
        y,
        width,
        height,
        hwnd_,
        nullptr,
        instance_,
        this);
    if (!trayPanelWindow_) {
        return;
    }

    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, 10, 10);
    SetWindowRgn(trayPanelWindow_, region, TRUE);
    trayPanelHoverItem_ = -1;
    ShowWindow(trayPanelWindow_, SW_SHOWNOACTIVATE);
    UpdateWindow(trayPanelWindow_);
    SetTimer(hwnd_, kTrayPanelCloseTimer, 50, nullptr);
}

void App::CloseTrayPanels() {
    if (hwnd_) {
        KillTimer(hwnd_, kTrayPanelCloseTimer);
    }
    if (devicePanelWindow_ && IsWindow(devicePanelWindow_)) {
        DestroyWindow(devicePanelWindow_);
    }
    devicePanelWindow_ = nullptr;
    if (trayPanelWindow_ && IsWindow(trayPanelWindow_)) {
        DestroyWindow(trayPanelWindow_);
    }
    trayPanelWindow_ = nullptr;
    trayPanelHoverItem_ = -1;
    devicePanelHoverItem_ = -1;
}

bool App::IsPointInsideTrayPanels(POINT pt) const {
    auto contains = [](HWND hwnd, POINT point) {
        if (!hwnd || !IsWindow(hwnd)) {
            return false;
        }
        RECT rect{};
        return GetWindowRect(hwnd, &rect) && PtInRect(&rect, point);
    };
    return contains(trayPanelWindow_, pt) || contains(devicePanelWindow_, pt);
}

void App::ShowDevicePanel() {
    if (!trayPanelWindow_ || !IsWindow(trayPanelWindow_)) {
        return;
    }
    const auto menuDevices = MenuDevices(devices_, configStore_.Get().showDisconnectedDevices);
    const int width = 270;
    const int height = std::max(52, 14 + std::max(1, static_cast<int>(menuDevices.size())) * 34);
    RECT trayRect{};
    GetWindowRect(trayPanelWindow_, &trayRect);
    RECT work{};
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    HMONITOR monitor = MonitorFromRect(&trayRect, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo)) {
        work = monitorInfo.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    }
    int x = trayRect.right + 6;
    if (x + width > work.right - 6) {
        x = trayRect.left - width - 6;
    }
    int y = std::clamp(static_cast<int>(trayRect.top), static_cast<int>(work.top) + 6, static_cast<int>(work.bottom) - height - 6);

    if (!devicePanelWindow_ || !IsWindow(devicePanelWindow_)) {
        devicePanelWindow_ = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
            kDevicePanelClass,
            L"BlueGauge 设备列表",
            WS_POPUP,
            x,
            y,
            width,
            height,
            hwnd_,
            nullptr,
            instance_,
            this);
        if (!devicePanelWindow_) {
            return;
        }
        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, 10, 10);
        SetWindowRgn(devicePanelWindow_, region, TRUE);
        ShowWindow(devicePanelWindow_, SW_SHOWNOACTIVATE);
    } else {
        SetWindowPos(devicePanelWindow_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    InvalidateRect(devicePanelWindow_, nullptr, TRUE);
}

void App::PaintTrayPanel(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rect{};
    GetClientRect(hwnd, &rect);

    FillRoundRect(hdc, rect, 10, kSurface);
    StrokeRoundRect(hdc, { rect.left, rect.top, rect.right - 1, rect.bottom - 1 }, 10, RGB(203, 213, 225));

    HFONT textFont = CreateUiFont(9);

    auto item = [&](int id, int y, const wchar_t* label, const wchar_t* right = L"", COLORREF color = kText) {
        RECT itemRect{ 8, y, rect.right - 8, y + 30 };
        if (trayPanelHoverItem_ == id) {
            FillRoundRect(hdc, itemRect, 8, RGB(239, 246, 255));
        }
        DrawTextLine(hdc, label, { 18, y, rect.right - 38, y + 30 },
            DT_SINGLELINE | DT_LEFT | DT_VCENTER, textFont, color);
        if (right && right[0] != L'\0') {
            DrawTextLine(hdc, right, { rect.right - 36, y, rect.right - 16, y + 30 },
                DT_SINGLELINE | DT_RIGHT | DT_VCENTER, textFont, color);
        }
    };

    item(kTrayPanelDevices, 8, L"设备列表", L">");
    item(kTrayPanelRefresh, 40, L"刷新");
    item(kTrayPanelSettings, 72, L"设置");
    DrawSeparator(hdc, 10, 106, rect.right - 10);
    item(kTrayPanelUpdate, 114, L"检查更新");
    item(kTrayPanelAbout, 146, L"关于");
    item(kTrayPanelExit, 178, L"退出");

    DeleteObject(textFont);
    EndPaint(hwnd, &ps);
}

void App::PaintDevicePanel(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rect{};
    GetClientRect(hwnd, &rect);

    FillRoundRect(hdc, rect, 10, kSurface);
    StrokeRoundRect(hdc, { rect.left, rect.top, rect.right - 1, rect.bottom - 1 }, 10, RGB(203, 213, 225));
    HFONT textFont = CreateUiFont(9);
    HFONT smallFont = CreateUiFont(8);

    const auto menuDevices = MenuDevices(devices_, configStore_.Get().showDisconnectedDevices);
    if (menuDevices.empty()) {
        DrawTextLine(hdc, L"暂无可显示设备", { 14, 8, rect.right - 14, rect.bottom - 8 },
            DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS, textFont, kSubtleText);
    } else {
        const int threshold = configStore_.Get().lowBatteryThreshold;
        for (int i = 0; i < static_cast<int>(menuDevices.size()); ++i) {
            const auto& device = menuDevices[static_cast<size_t>(i)];
            const int y = 8 + i * 34;
            RECT itemRect{ 8, y, rect.right - 8, y + 30 };
            if (devicePanelHoverItem_ == i) {
                FillRoundRect(hdc, itemRect, 8, RGB(239, 246, 255));
            }
            const COLORREF stateColor = device.connected ? kText : kSubtleText;
            DrawTextLine(hdc, device.name, { 16, y + 1, rect.right - 76, y + 16 },
                DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS, textFont, stateColor);
            DrawTextLine(hdc, device.connected ? L"已连接" : L"未连接", { 16, y + 15, rect.right - 76, y + 30 },
                DT_SINGLELINE | DT_LEFT | DT_VCENTER, smallFont, device.connected ? RGB(22, 163, 74) : kSubtleText);
            std::wstring batteryText = device.batteryPercent.has_value() ? std::to_wstring(*device.batteryPercent) + L"%" : L"未知";
            const COLORREF batteryColor = device.batteryPercent.has_value() ? BatteryColor(*device.batteryPercent, threshold) : kSubtleText;
            DrawTextLine(hdc, batteryText, { rect.right - 70, y, rect.right - 16, y + 30 },
                DT_SINGLELINE | DT_RIGHT | DT_VCENTER, textFont, batteryColor);
        }
    }

    DeleteObject(textFont);
    DeleteObject(smallFont);
    EndPaint(hwnd, &ps);
}

void App::HandleTrayPanelMouse(HWND hwnd, int x, int y) {
    (void)x;
    int hover = -1;
    if (y >= 8 && y < 38) {
        hover = kTrayPanelDevices;
    } else if (y >= 40 && y < 70) {
        hover = kTrayPanelRefresh;
    } else if (y >= 72 && y < 102) {
        hover = kTrayPanelSettings;
    } else if (y >= 114 && y < 144) {
        hover = kTrayPanelUpdate;
    } else if (y >= 146 && y < 176) {
        hover = kTrayPanelAbout;
    } else if (y >= 178 && y < 208) {
        hover = kTrayPanelExit;
    }
    if (trayPanelHoverItem_ != hover) {
        trayPanelHoverItem_ = hover;
        InvalidateRect(hwnd, nullptr, TRUE);
    }
    if (hover == kTrayPanelDevices) {
        ShowDevicePanel();
    } else if (devicePanelWindow_ && IsWindow(devicePanelWindow_)) {
        DestroyWindow(devicePanelWindow_);
        devicePanelWindow_ = nullptr;
        devicePanelHoverItem_ = -1;
    }
    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
    TrackMouseEvent(&tme);
}

void App::HandleDevicePanelMouse(HWND hwnd, int x, int y) {
    (void)x;
    const auto menuDevices = MenuDevices(devices_, configStore_.Get().showDisconnectedDevices);
    int hover = -1;
    if (y >= 8) {
        const int index = (y - 8) / 34;
        if (index >= 0 && index < static_cast<int>(menuDevices.size()) && y < 8 + index * 34 + 30) {
            hover = index;
        }
    }
    if (devicePanelHoverItem_ != hover) {
        devicePanelHoverItem_ = hover;
        InvalidateRect(hwnd, nullptr, TRUE);
    }
    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, hwnd, 0 };
    TrackMouseEvent(&tme);
}

void App::HandleDevicePanelClick(HWND hwnd, int x, int y) {
    (void)hwnd;
    (void)x;
    (void)y;
}

void App::HandleTrayPanelClick(HWND hwnd, int x, int y) {
    (void)x;
    (void)hwnd;

    if (y >= 40 && y < 70) {
        CloseTrayPanels();
        RefreshAsync();
        return;
    }
    if (y >= 72 && y < 102) {
        CloseTrayPanels();
        ShowSettings();
        return;
    }
    if (y >= 114 && y < 144) {
        CloseTrayPanels();
        ShowUpdateDialog(true);
        return;
    }
    if (y >= 146 && y < 176) {
        CloseTrayPanels();
        ShowAbout();
        return;
    }
    if (y >= 178 && y < 208) {
        CloseTrayPanels();
        DestroyWindow(hwnd_);
        return;
    }
}

void App::CheckLowBattery() {
    if (!configStore_.Get().enableLowBatteryNotify) {
        return;
    }
    for (const auto& device : devices_) {
        if (!device.connected || !device.batteryPercent.has_value()) {
            continue;
        }
        const bool low = *device.batteryPercent < configStore_.Get().lowBatteryThreshold;
        if (!low) {
            lowBatteryNotified_.erase(device.id);
            continue;
        }
        if (lowBatteryNotified_.insert(device.id).second) {
            std::wstring message = device.name + L" 电量低: " + std::to_wstring(*device.batteryPercent) + L"%";
            Logger::Instance().Warn(L"低电量提醒: " + message);
            ShowBalloon(hwnd_, L"蓝牙设备低电量", message);
        }
    }
}

void App::TrackConnectionChanges(const std::vector<BluetoothDeviceInfo>& newDevices) {
    std::map<std::wstring, bool> nextStates;
    for (const auto& device : newDevices) {
        if (!device.id.empty()) {
            nextStates[device.id] = device.connected;
        }
    }

    if (!hasScanBaseline_) {
        previousConnectionStates_ = std::move(nextStates);
        hasScanBaseline_ = true;
        return;
    }

    const bool notifyEnabled = configStore_.Get().enableConnectionNotify;
    const DWORD now = GetTickCount();
    for (const auto& device : newDevices) {
        auto previous = previousConnectionStates_.find(device.id);
        if (previous == previousConnectionStates_.end() || previous->second == device.connected) {
            continue;
        }

        if (!notifyEnabled) {
            continue;
        }

        const std::wstring debounceKey = device.id + (device.connected ? L"|1" : L"|0");
        auto last = recentConnectionNotifyTicks_.find(debounceKey);
        if (last != recentConnectionNotifyTicks_.end() && now - last->second < 5000) {
            continue;
        }
        recentConnectionNotifyTicks_[debounceKey] = now;
        QueueConnectionToast(device.name + (device.connected ? L" 已连接" : L" 已断开"));
    }

    previousConnectionStates_ = std::move(nextStates);
}

void App::QueueConnectionToast(const std::wstring& text) {
    connectionToastQueue_.push_back(text);
    ShowNextConnectionToast();
}

void App::ShowNextConnectionToast() {
    if (connectionToastWindow_ && IsWindow(connectionToastWindow_)) {
        return;
    }
    if (connectionToastQueue_.empty()) {
        return;
    }
    currentConnectionToastText_ = std::move(connectionToastQueue_.front());
    connectionToastQueue_.pop_front();
    CreateConnectionToastWindow();
}

bool App::CreateConnectionToastWindow() {
    WNDCLASSW wc{};
    wc.lpfnWndProc = App::ConnectionToastWindowProc;
    wc.hInstance = instance_;
    wc.lpszClassName = kConnectionToastClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    RegisterClassW(&wc);

    constexpr int width = 286;
    constexpr int height = 64;
    RECT work{};
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo)) {
        work = monitorInfo.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    }
    const int x = static_cast<int>(work.right) - width - 18;
    const int y = static_cast<int>(work.bottom) - height - 18;

    connectionToastWindow_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        kConnectionToastClass,
        L"BlueGauge 连接提醒",
        WS_POPUP,
        x,
        y,
        width,
        height,
        hwnd_,
        nullptr,
        instance_,
        this);
    if (!connectionToastWindow_) {
        return false;
    }

    HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1, 12, 12);
    SetWindowRgn(connectionToastWindow_, region, TRUE);
    ShowWindow(connectionToastWindow_, SW_SHOWNOACTIVATE);
    UpdateWindow(connectionToastWindow_);
    SetTimer(connectionToastWindow_, kConnectionToastTimer, kConnectionToastDurationMs, nullptr);
    return true;
}

void App::CloseConnectionToastWindow() {
    if (connectionToastWindow_ && IsWindow(connectionToastWindow_)) {
        DestroyWindow(connectionToastWindow_);
    }
    connectionToastWindow_ = nullptr;
}

void App::PaintConnectionToastWindow(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rect{};
    GetClientRect(hwnd, &rect);

    FillRoundRect(hdc, rect, 10, kSurface);
    StrokeRoundRect(hdc, { rect.left, rect.top, rect.right - 1, rect.bottom - 1 }, 10, kBorder);
    FillRoundRect(hdc, { 14, 19, 18, 45 }, 4, kBlue);

    HFONT titleFont = CreateUiFont(9, FW_SEMIBOLD);
    HFONT smallFont = CreateUiFont(8);
    DrawTextLine(hdc, L"BlueGauge", { 28, 12, rect.right - 16, 30 },
        DT_SINGLELINE | DT_LEFT | DT_VCENTER, smallFont, kSubtleText);
    DrawTextLine(hdc, currentConnectionToastText_, { 28, 30, rect.right - 16, 54 },
        DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS, titleFont, kText);
    DeleteObject(titleFont);
    DeleteObject(smallFont);
    EndPaint(hwnd, &ps);
}

void App::ShowSettings() {
    if (settingsWindow_ && IsWindow(settingsWindow_)) {
        ShowWindow(settingsWindow_, SW_SHOWNORMAL);
        SetWindowPos(settingsWindow_, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(settingsWindow_);
        return;
    }

    const AppConfig original = configStore_.Get();
    activeSettingsSection_ = SectionScan;
    settingsScrollY_ = 0;

    WNDCLASSW wc{};
    wc.lpfnWndProc = App::SettingsWindowProc;
    wc.hInstance = instance_;
    wc.lpszClassName = kSettingsClass;
    wc.hIcon = LoadIconW(instance_, MAKEINTRESOURCEW(IDI_APP));
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    RegisterClassW(&wc);

    constexpr int desiredDialogWidth = 800;
    constexpr int desiredDialogHeight = 740;
    RECT work{};
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo)) {
        work = monitorInfo.rcWork;
    } else {
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    }
    const int dialogWidth = std::min(desiredDialogWidth, std::max(720, static_cast<int>(work.right - work.left) - 24));
    const int availableHeight = std::max(620, static_cast<int>(work.bottom - work.top) - 24);
    const int dialogHeight = std::min(desiredDialogHeight, availableHeight);
    const int dialogX = static_cast<int>(work.left) + (static_cast<int>(work.right - work.left) - dialogWidth) / 2;
    const int dialogY = static_cast<int>(work.top) + (static_cast<int>(work.bottom - work.top) - dialogHeight) / 2;

    HWND dialog = CreateWindowExW(WS_EX_APPWINDOW | WS_EX_CONTROLPARENT, kSettingsClass, L"BlueGauge 设置",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, dialogX, dialogY, dialogWidth, dialogHeight,
        nullptr, nullptr, instance_, this);
    if (!dialog) {
        return;
    }
    settingsWindow_ = dialog;

    HICON smallIcon = reinterpret_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 16, 16, LR_DEFAULTCOLOR | LR_SHARED));
    HICON bigIcon = reinterpret_cast<HICON>(LoadImageW(instance_, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, 32, 32, LR_DEFAULTCOLOR | LR_SHARED));
    SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(smallIcon));
    SendMessageW(dialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(bigIcon));

    auto add = [&](DWORD exStyle, const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
        HWND child = CreateWindowExW(exStyle, cls, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, x, y, w, h, dialog,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (!settingsControlFont_) {
            settingsControlFont_ = CreateUiFont(10);
        }
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(settingsControlFont_), TRUE);
        return child;
    };

    HWND refreshEdit = add(0, L"EDIT", std::to_wstring(original.refreshIntervalSeconds).c_str(), ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL,
        IDC_REFRESH, 486, 140, 118, 28);
    HWND thresholdEdit = add(0, L"EDIT", std::to_wstring(original.lowBatteryThreshold).c_str(), ES_NUMBER | ES_RIGHT | ES_AUTOHSCROLL,
        IDC_THRESHOLD, 486, 140, 78, 28);
    SendMessageW(refreshEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 8));
    SendMessageW(thresholdEdit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(6, 8));
    LayoutSettingsControls(dialog);

    ShowWindow(dialog, SW_SHOW);
    SetForegroundWindow(dialog);
}

void App::ShowAbout() {
    if (aboutWindow_ && IsWindow(aboutWindow_)) {
        ShowWindow(aboutWindow_, IsIconic(aboutWindow_) ? SW_RESTORE : SW_SHOWNORMAL);
        SetWindowPos(aboutWindow_, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        SetForegroundWindow(aboutWindow_);
        return;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc = App::AboutWindowProc;
    wc.hInstance = instance_;
    wc.lpszClassName = kAboutClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
    RegisterClassW(&wc);

    constexpr int width = 560;
    constexpr int height = 390;
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int x = work.left + (work.right - work.left - width) / 2;
    const int y = work.top + (work.bottom - work.top - height) / 2;
    aboutWindow_ = CreateWindowExW(WS_EX_TOOLWINDOW, kAboutClass, L"关于 BlueGauge",
        WS_POPUP, x, y, width, height, hwnd_, nullptr, instance_, this);
    if (aboutWindow_) {
        ShowWindow(aboutWindow_, SW_SHOW);
        UpdateWindow(aboutWindow_);
    }
}

void App::PaintAboutWindow(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rect{};
    GetClientRect(hwnd, &rect);
    FillRoundRect(hdc, rect, 8, kSurface);
    StrokeRoundRect(hdc, { rect.left, rect.top, rect.right - 1, rect.bottom - 1 }, 8, RGB(203, 213, 225));

    HFONT titleFont = CreateUiFont(15, FW_SEMIBOLD);
    HFONT textFont = CreateUiFont(10);
    HFONT smallFont = CreateUiFont(9);

    DrawTextLine(hdc, kAppName, { 28, 22, rect.right - 28, 54 }, DT_SINGLELINE | DT_LEFT | DT_VCENTER, titleFont, kText);
    DrawTextLine(hdc, L"Windows 10/11 原生蓝牙电量监控工具", { 28, 58, rect.right - 28, 82 },
        DT_SINGLELINE | DT_LEFT | DT_VCENTER, textFont, kSubtleText);
    DrawSeparator(hdc, 28, 98, rect.right - 28);

    DrawTextLine(hdc, std::wstring(L"版本：") + kAppVersion + L"  " + GetBuildConfig(), { 28, 114, rect.right - 28, 140 },
        DT_SINGLELINE | DT_LEFT | DT_VCENTER, textFont, kText);
    DrawTextLine(hdc, std::wstring(L"构建时间：") + GetBuildTimestamp(), { 28, 144, rect.right - 28, 170 },
        DT_SINGLELINE | DT_LEFT | DT_VCENTER, textFont, kText);
    DrawTextLine(hdc, std::wstring(L"运行路径：") + GetExecutablePath(), { 28, 174, rect.right - 28, 200 },
        DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS, textFont, kText);
    DrawTextLine(hdc, std::wstring(L"作者：") + kAppAuthor, { 28, 204, rect.right - 28, 230 },
        DT_SINGLELINE | DT_LEFT | DT_VCENTER, textFont, kText);
    DrawTextLine(hdc, L"开源协议：MIT License", { 28, 234, rect.right - 28, 260 },
        DT_SINGLELINE | DT_LEFT | DT_VCENTER, textFont, kText);
    DrawTextLine(hdc, L"项目链接：HackenLeung/BlueGauge", { 28, 264, rect.right - 28, 290 },
        DT_SINGLELINE | DT_LEFT | DT_VCENTER, textFont, kBlue);

    DrawButtonFrame(hdc, { rect.right - 238, rect.bottom - 54, rect.right - 128, rect.bottom - 22 }, L"检查更新", smallFont, true);
    DrawButtonFrame(hdc, { rect.right - 116, rect.bottom - 54, rect.right - 28, rect.bottom - 22 }, L"关闭", smallFont, false);

    DeleteObject(titleFont);
    DeleteObject(textFont);
    DeleteObject(smallFont);
    EndPaint(hwnd, &ps);
}

void App::HandleAboutClick(HWND hwnd, int x, int y) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    if (PointInRect(x, y, { 28, 264, rect.right - 28, 290 })) {
        ShellExecuteW(hwnd, L"open", kProjectUrl, nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (PointInRect(x, y, { rect.right - 238, rect.bottom - 54, rect.right - 128, rect.bottom - 22 })) {
        ShowUpdateDialog(true);
        return;
    }
    if (PointInRect(x, y, { rect.right - 116, rect.bottom - 54, rect.right - 28, rect.bottom - 22 })) {
        DestroyWindow(hwnd);
        return;
    }
}

void App::ShowUpdateDialog(bool startCheck) {
    if (!updateWindow_ || !IsWindow(updateWindow_)) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = App::UpdateWindowProc;
        wc.hInstance = instance_;
        wc.lpszClassName = kUpdateClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
        RegisterClassW(&wc);

        constexpr int width = 440;
        constexpr int height = 260;
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int x = work.left + (work.right - work.left - width) / 2;
        const int y = work.top + (work.bottom - work.top - height) / 2;
        updateWindow_ = CreateWindowExW(WS_EX_TOOLWINDOW, kUpdateClass, L"检查更新",
            WS_POPUP, x, y, width, height, hwnd_, nullptr, instance_, this);
    }
    if (!updateWindow_) {
        return;
    }
    ShowWindow(updateWindow_, SW_SHOW);
    SetWindowPos(updateWindow_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    if (startCheck) {
        if (updateStatus_ == UpdateChecking) {
            InvalidateRect(updateWindow_, nullptr, TRUE);
            return;
        }
        updateStatus_ = UpdateChecking;
        updateTitle_ = L"正在检查更新";
        updateMessage_ = L"正在从 GitHub Releases 获取最新版本...";
        updatePrimaryText_.clear();
        updatePrimaryUrl_.clear();
        InvalidateRect(updateWindow_, nullptr, TRUE);
        CheckForUpdatesAsync();
    } else {
        InvalidateRect(updateWindow_, nullptr, TRUE);
    }
}

void App::PaintUpdateWindow(HWND hwnd) {
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    RECT rect{};
    GetClientRect(hwnd, &rect);
    FillRoundRect(hdc, rect, 8, kSurface);
    StrokeRoundRect(hdc, { rect.left, rect.top, rect.right - 1, rect.bottom - 1 }, 8, RGB(203, 213, 225));

    HFONT titleFont = CreateUiFont(13, FW_SEMIBOLD);
    HFONT textFont = CreateUiFont(10);
    HFONT smallFont = CreateUiFont(9);
    DrawTextLine(hdc, updateTitle_.empty() ? L"检查更新" : updateTitle_, { 28, 24, rect.right - 28, 54 },
        DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS, titleFont, kText);
    DrawTextLine(hdc, std::wstring(L"当前版本：") + kAppVersion, { 28, 70, rect.right - 28, 96 },
        DT_SINGLELINE | DT_LEFT | DT_VCENTER, textFont, kSubtleText);
    DrawTextLine(hdc, updateMessage_.empty() ? L"点击检查更新后，将打开 GitHub Releases 页面下载新版。" : updateMessage_,
        { 28, 108, rect.right - 28, 156 }, DT_WORDBREAK | DT_LEFT | DT_VCENTER, textFont, kText);

    if (!updatePrimaryText_.empty()) {
        DrawButtonFrame(hdc, { rect.right - 238, rect.bottom - 54, rect.right - 128, rect.bottom - 22 }, updatePrimaryText_, smallFont, true);
    }
    DrawButtonFrame(hdc, { rect.right - 116, rect.bottom - 54, rect.right - 28, rect.bottom - 22 }, L"关闭", smallFont, false);

    DeleteObject(titleFont);
    DeleteObject(textFont);
    DeleteObject(smallFont);
    EndPaint(hwnd, &ps);
}

void App::HandleUpdateClick(HWND hwnd, int x, int y) {
    RECT rect{};
    GetClientRect(hwnd, &rect);
    if (!updatePrimaryText_.empty() && PointInRect(x, y, { rect.right - 238, rect.bottom - 54, rect.right - 128, rect.bottom - 22 })) {
        ShellExecuteW(hwnd, L"open", updatePrimaryUrl_.empty() ? kProjectUrl : updatePrimaryUrl_.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return;
    }
    if (PointInRect(x, y, { rect.right - 116, rect.bottom - 54, rect.right - 28, rect.bottom - 22 })) {
        DestroyWindow(hwnd);
        return;
    }
}

void App::CheckForUpdatesAsync() {
    updateService_.CheckAsync(hwnd_);
}

void App::ToggleStartup() {
    auto& cfg = configStore_.Edit();
    cfg.startWithWindows = !IsStartupEnabled();
    SetStartupEnabled(cfg.startWithWindows);
    configStore_.Save();
}

void App::Shutdown() {
    KillTimer(hwnd_, kRefreshTimer);
    KillTimer(hwnd_, kBluetoothChangeDebounceTimer);
    bluetoothWatcher_.Stop();
    tray_.Remove();
    CloseTrayPanels();
    CloseConnectionToastWindow();
    if (statusWindow_) {
        DestroyWindow(statusWindow_);
        statusWindow_ = nullptr;
    }
    if (aboutWindow_) {
        DestroyWindow(aboutWindow_);
        aboutWindow_ = nullptr;
    }
    if (updateWindow_) {
        DestroyWindow(updateWindow_);
        updateWindow_ = nullptr;
    }
    scannerService_.Shutdown();
    updateService_.Shutdown();
    Logger::Instance().Info(L"程序退出");
}

LRESULT CALLBACK App::WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = nullptr;
    if (message == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = reinterpret_cast<App*>(cs->lpCreateParams);
        app->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
        return DefWindowProcW(hwnd, message, wParam, lParam);
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return app ? app->HandleMessage(message, wParam, lParam) : DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK App::SettingsWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = nullptr;
    if (message == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = reinterpret_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!app) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        app->PaintSettingsWindow(hwnd);
        return 0;
    case WM_SIZE:
        app->LayoutSettingsControls(hwnd);
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_LBUTTONUP: {
        const int x = GET_X_LPARAM(lParam);
        const int y = GET_Y_LPARAM(lParam);
        RECT rect{};
        GetClientRect(hwnd, &rect);
        if (x >= 24 && x <= 154) {
            if (y >= 81 && y <= 117) {
                app->SetSettingsSection(hwnd, SectionScan);
                return 0;
            }
            if (y >= 131 && y <= 167) {
                app->SetSettingsSection(hwnd, SectionAlerts);
                return 0;
            }
            if (y >= 181 && y <= 217) {
                app->SetSettingsSection(hwnd, SectionDisplay);
                return 0;
            }
            if (y >= 231 && y <= 267) {
                app->SetSettingsSection(hwnd, SectionSystem);
                return 0;
            }
        }
        const int left = 220;
        const int right = rect.right - 38;
        const int top = 82;
        auto& cfg = app->configStore_.Edit();
        if (app->activeSettingsSection_ == SectionAlerts) {
            const int thresholdIndex = HitSettingsSegment(SettingsSegmentedRect(right, top + 55), x, y);
            if (thresholdIndex >= 0) {
                cfg.lowBatteryThreshold = kThresholdOptions[thresholdIndex];
                app->configStore_.Save();
                app->SyncBatteryWindow();
                app->CheckLowBattery();
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            if (PointInRect(x, y, SettingsCheckHitRect(right, top + 154, top + 210))) {
                cfg.enableLowBatteryNotify = !cfg.enableLowBatteryNotify;
                app->configStore_.Save();
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            if (PointInRect(x, y, SettingsCheckHitRect(right, top + 254, top + 310))) {
                cfg.enableConnectionNotify = !cfg.enableConnectionNotify;
                app->configStore_.Save();
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
        } else if (app->activeSettingsSection_ == SectionDisplay) {
            for (int i = 0; i < kTaskbarBatteryStyleCount; ++i) {
                RECT card = DisplayStyleCardRect(left, right, top, i);
                if (PointInRect(x, y, card)) {
                    cfg.taskbarBatteryStyle = i;
                    app->configStore_.Save();
                    app->SyncBatteryWindow();
                    InvalidateRect(hwnd, nullptr, TRUE);
                    return 0;
                }
            }
            for (int i = 1; i <= 3; ++i) {
                RECT seg{ right - 168 + (i - 1) * 54, top + kDisplayMaxDevicesTop + 6, right - 168 + (i - 1) * 54 + 50, top + kDisplayMaxDevicesTop + 38 };
                if (PointInRect(x, y, seg)) {
                    cfg.taskbarMaxDevices = i;
                    app->configStore_.Save();
                    app->SyncBatteryWindow();
                    InvalidateRect(hwnd, nullptr, TRUE);
                    return 0;
                }
            }
            if (PointInRect(x, y, SettingsCheckHitRect(right, top + 48, top + 100))) {
                cfg.showDisconnectedDevices = !cfg.showDisconnectedDevices;
                app->configStore_.Save();
                app->RefreshAsync();
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            const int deviceCount = std::min(3, static_cast<int>(app->devices_.size()));
            for (int i = 0; i < deviceCount; ++i) {
                RECT row{ left, top + kDisplayPinnedRowsTop + i * 34, right, top + kDisplayPinnedRowsTop + i * 34 + 30 };
                if (PointInRect(x, y, row)) {
                    const std::wstring id = app->devices_[static_cast<size_t>(i)].id;
                    auto it = std::find(cfg.pinnedDeviceIds.begin(), cfg.pinnedDeviceIds.end(), id);
                    if (it != cfg.pinnedDeviceIds.end()) {
                        cfg.pinnedDeviceIds.erase(it);
                    } else if (cfg.pinnedDeviceIds.size() < kMaxPinnedDevices) {
                        cfg.pinnedDeviceIds.push_back(id);
                    }
                    app->configStore_.Save();
                    app->SyncBatteryWindow();
                    InvalidateRect(hwnd, nullptr, TRUE);
                    return 0;
                }
            }
            if (PointInRect(x, y, { right - 98, top + kDisplayRefreshButtonTop, right, top + kDisplayRefreshButtonTop + 32 })) {
                app->RefreshAsync();
                return 0;
            }
        } else if (app->activeSettingsSection_ == SectionSystem) {
            if (PointInRect(x, y, SettingsCheckHitRect(right, top + 58, top + 112))) {
                cfg.startWithWindows = !cfg.startWithWindows;
                SetStartupEnabled(cfg.startWithWindows);
                app->configStore_.Save();
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
        } else if (app->activeSettingsSection_ == SectionScan) {
            const int refreshIndex = HitSettingsSegment(SettingsSegmentedRect(right, top + 55), x, y);
            if (refreshIndex >= 0) {
                cfg.refreshIntervalSeconds = kRefreshOptions[refreshIndex];
                app->configStore_.Save();
                app->StartTimer();
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
            if (PointInRect(x, y, ScanPrimaryButtonRect(right, top))) {
                if (app->scanInProgress_) {
                    return 0;
                }
                app->RefreshAsync();
                InvalidateRect(hwnd, nullptr, TRUE);
                return 0;
            }
        }
        break;
    }
    case WM_MOUSEWHEEL: {
        (void)wParam;
        return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetBkColor(reinterpret_cast<HDC>(wParam), kSurface);
        SetTextColor(reinterpret_cast<HDC>(wParam), kText);
        return reinterpret_cast<LRESULT>(GetStockObject(WHITE_BRUSH));
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_REFRESH || LOWORD(wParam) == IDC_THRESHOLD) {
            const int id = LOWORD(wParam);
            const int code = HIWORD(wParam);
            auto& cfg = app->configStore_.Edit();

            int value = 0;
            if (!TryReadInt(hwnd, id, value)) {
                if (code == EN_KILLFOCUS) {
                    SetIntText(hwnd, id, id == IDC_REFRESH ? cfg.refreshIntervalSeconds : cfg.lowBatteryThreshold);
                }
                return 0;
            }

            if (id == IDC_REFRESH) {
                if (code != EN_KILLFOCUS && (value < 5 || value > 3600)) {
                    return 0;
                }
                const int clamped = std::clamp(value, 5, 3600);
                if (code == EN_KILLFOCUS && clamped != value) {
                    SetIntText(hwnd, IDC_REFRESH, clamped);
                }
                if (cfg.refreshIntervalSeconds != clamped) {
                    cfg.refreshIntervalSeconds = clamped;
                    app->configStore_.Save();
                    app->StartTimer();
                }
                return 0;
            }

            if (code != EN_KILLFOCUS && (value < 1 || value > 100)) {
                return 0;
            }
            const int clamped = std::clamp(value, 1, 100);
            if (code == EN_KILLFOCUS && clamped != value) {
                SetIntText(hwnd, IDC_THRESHOLD, clamped);
            }
            if (cfg.lowBatteryThreshold != clamped) {
                cfg.lowBatteryThreshold = clamped;
                app->configStore_.Save();
                app->SyncBatteryWindow();
                app->CheckLowBattery();
            }
            return 0;
        }
        break;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (app->settingsWindow_ == hwnd) {
            app->settingsWindow_ = nullptr;
        }
        if (app->settingsControlFont_) {
            DeleteObject(app->settingsControlFont_);
            app->settingsControlFont_ = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK App::StatusWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = nullptr;
    if (message == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = reinterpret_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (app) {
            app->PaintStatusWindow(hwnd);
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        if (app) {
            RECT rect{};
            GetClientRect(hwnd, &rect);
            const int x = GET_X_LPARAM(lParam);
            const int y = GET_Y_LPARAM(lParam);
            if (x >= rect.right - 86 && y >= rect.bottom - 42) {
                app->RefreshAsync();
                return 0;
            }
        }
        break;
    case WM_RBUTTONUP:
        if (app) {
            app->ShowTrayPanel();
            return 0;
        }
        break;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_WINDOWPOSCHANGED:
        if (app && !app->positioningStatusWindow_) {
            app->PositionStatusWindow();
        }
        break;
    case WM_DESTROY:
        if (app && app->statusWindow_ == hwnd) {
            app->statusWindow_ = nullptr;
        }
        return 0;
    case WM_NCHITTEST:
        return HTCLIENT;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK App::TrayPanelWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = nullptr;
    if (message == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = reinterpret_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!app) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        app->PaintTrayPanel(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        app->HandleTrayPanelMouse(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_MOUSELEAVE:
        app->trayPanelHoverItem_ = -1;
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_LBUTTONUP:
        app->HandleTrayPanelClick(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            app->CloseTrayPanels();
            return 0;
        }
        break;
    case WM_CANCELMODE:
        app->CloseTrayPanels();
        return 0;
    case WM_DESTROY:
        if (app->trayPanelWindow_ == hwnd) {
            app->trayPanelWindow_ = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK App::DevicePanelWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = nullptr;
    if (message == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = reinterpret_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!app) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        app->PaintDevicePanel(hwnd);
        return 0;
    case WM_MOUSEMOVE:
        app->HandleDevicePanelMouse(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_MOUSELEAVE:
        app->devicePanelHoverItem_ = -1;
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    case WM_LBUTTONUP:
        app->HandleDevicePanelClick(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_DESTROY:
        if (app->devicePanelWindow_ == hwnd) {
            app->devicePanelWindow_ = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK App::AboutWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = nullptr;
    if (message == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = reinterpret_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!app) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        app->PaintAboutWindow(hwnd);
        return 0;
    case WM_LBUTTONUP:
        app->HandleAboutClick(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (app->aboutWindow_ == hwnd) {
            app->aboutWindow_ = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK App::UpdateWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = nullptr;
    if (message == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = reinterpret_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!app) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        app->PaintUpdateWindow(hwnd);
        return 0;
    case WM_LBUTTONUP:
        app->HandleUpdateClick(hwnd, GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (app->updateWindow_ == hwnd) {
            app->updateWindow_ = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT CALLBACK App::ConnectionToastWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    App* app = nullptr;
    if (message == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        app = reinterpret_cast<App*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    } else {
        app = reinterpret_cast<App*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }

    if (!app) {
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        app->PaintConnectionToastWindow(hwnd);
        return 0;
    case WM_TIMER:
        if (wParam == kConnectionToastTimer) {
            KillTimer(hwnd, kConnectionToastTimer);
            app->CloseConnectionToastWindow();
            app->ShowNextConnectionToast();
            return 0;
        }
        break;
    case WM_LBUTTONUP:
        app->CloseConnectionToastWindow();
        app->ShowNextConnectionToast();
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_NCHITTEST:
        return HTCLIENT;
    case WM_DESTROY:
        if (app->connectionToastWindow_ == hwnd) {
            app->connectionToastWindow_ = nullptr;
        }
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

LRESULT App::HandleMessage(UINT message, WPARAM wParam, LPARAM lParam) {
    if (taskbarCreatedMessage_ != 0 && message == taskbarCreatedMessage_) {
        Logger::Instance().Info(L"检测到任务栏重启，恢复托盘和任务栏电量显示");
        taskbarWindow_ = nullptr;
        if (statusWindow_ && IsWindow(statusWindow_)) {
            DestroyWindow(statusWindow_);
            statusWindow_ = nullptr;
        }
        tray_.Remove();
        tray_.Add(hwnd_, instance_);
        UpdateTray();
        SyncBatteryWindow();
        return 0;
    }

    switch (message) {
    case WM_TIMER:
        if (wParam == kRefreshTimer) {
            RefreshAsync();
        } else if (wParam == kBluetoothChangeDebounceTimer) {
            RefreshFromBluetoothChange();
        } else if (wParam == kTrayPanelCloseTimer) {
            if ((!trayPanelWindow_ || !IsWindow(trayPanelWindow_))
                && (!devicePanelWindow_ || !IsWindow(devicePanelWindow_))) {
                KillTimer(hwnd_, kTrayPanelCloseTimer);
                return 0;
            }
            if ((GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0) {
                CloseTrayPanels();
                return 0;
            }
            const bool mouseDown = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0
                || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0
                || (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
            if (mouseDown) {
                POINT pt{};
                GetCursorPos(&pt);
                if (!IsPointInsideTrayPanels(pt)) {
                    CloseTrayPanels();
                }
            }
        }
        return 0;
    case WM_ACTIVATEAPP:
        if (!wParam) {
            CloseTrayPanels();
        }
        return 0;
    case WM_SCAN_COMPLETE:
        ApplyScanResult(reinterpret_cast<std::vector<BluetoothDeviceInfo>*>(lParam));
        return 0;
    case WM_BLUETOOTH_DEVICE_CHANGED:
        ScheduleBluetoothChangeRefresh(L"收到 WinRT 蓝牙设备变化事件");
        return 0;
    case WM_UPDATE_RESULT: {
        std::unique_ptr<UpdateResult> result(reinterpret_cast<UpdateResult*>(lParam));
        if (result) {
            updateStatus_ = result->status;
            updatePrimaryText_.clear();
            updatePrimaryUrl_.clear();
            switch (result->status) {
            case UpdateLatest:
                updateTitle_ = L"已是最新版本";
                updateMessage_ = std::wstring(L"当前版本 ") + kAppVersion + L" 已是最新版本。";
                break;
            case UpdateAvailable:
                updateTitle_ = L"发现新版本";
                updateMessage_ = std::wstring(L"当前版本：") + kAppVersion + L"\n最新版本：" + result->latestVersion;
                updatePrimaryText_ = L"打开下载页";
                updatePrimaryUrl_ = result->htmlUrl;
                break;
            case UpdateNoRelease:
                updateTitle_ = L"暂未发布可用版本";
                updateMessage_ = result->message;
                updatePrimaryText_ = L"打开项目页";
                updatePrimaryUrl_ = kProjectUrl;
                break;
            default:
                updateTitle_ = L"检查更新失败";
                updateMessage_ = result->message.empty() ? L"检查更新失败，请稍后重试。" : result->message;
                updatePrimaryText_ = L"打开项目页";
                updatePrimaryUrl_ = kProjectUrl;
                break;
            }
        }
        if (updateWindow_ && IsWindow(updateWindow_)) {
            InvalidateRect(updateWindow_, nullptr, TRUE);
        }
        updateService_.CompleteCheck();
        return 0;
    }
    case WM_TRAYICON:
        if (LOWORD(lParam) == WM_RBUTTONUP) {
            ShowTrayPanel();
        } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            RefreshAsync();
        }
        return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
        SyncBatteryWindow();
        return 0;
    case WM_DEVICECHANGE:
        if (wParam == DBT_DEVNODES_CHANGED
            || wParam == DBT_DEVICEARRIVAL
            || wParam == DBT_DEVICEREMOVECOMPLETE) {
            ScheduleBluetoothChangeRefresh(L"收到 WM_DEVICECHANGE");
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case IDM_REFRESH:
            RefreshAsync();
            return 0;
        case IDM_SETTINGS:
            ShowSettings();
            return 0;
        case IDM_CHECK_UPDATE:
            ShowUpdateDialog(true);
            return 0;
        case IDM_ABOUT:
            ShowAbout();
            return 0;
        case IDM_EXIT:
            DestroyWindow(hwnd_);
            return 0;
        default:
            return 0;
        }
    case WM_DESTROY:
        Shutdown();
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd_, message, wParam, lParam);
    }
}
