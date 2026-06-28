#include "services/update_service.h"

#include "logger.h"
#include "tray.h"
#include "version.h"

#include <Winhttp.h>
#include <algorithm>
#include <memory>
#include <optional>
#include <vector>

namespace {
std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (size <= 0) {
        return {};
    }
    std::wstring result(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::optional<std::string> FindJsonString(const std::string& json, const std::string& key) {
    const std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = json.find(':', pos + pattern.size());
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    pos = json.find('"', pos + 1);
    if (pos == std::string::npos) {
        return std::nullopt;
    }
    std::string value;
    for (++pos; pos < json.size(); ++pos) {
        const char ch = json[pos];
        if (ch == '\\' && pos + 1 < json.size()) {
            value.push_back(json[++pos]);
            continue;
        }
        if (ch == '"') {
            return value;
        }
        value.push_back(ch);
    }
    return std::nullopt;
}

std::vector<int> VersionParts(std::wstring version) {
    if (!version.empty() && (version[0] == L'v' || version[0] == L'V')) {
        version.erase(version.begin());
    }
    std::vector<int> parts;
    size_t start = 0;
    while (start < version.size()) {
        size_t end = version.find(L'.', start);
        if (end == std::wstring::npos) {
            end = version.size();
        }
        int value = 0;
        for (size_t i = start; i < end; ++i) {
            if (version[i] >= L'0' && version[i] <= L'9') {
                value = value * 10 + static_cast<int>(version[i] - L'0');
            } else {
                break;
            }
        }
        parts.push_back(value);
        start = end + 1;
    }
    return parts;
}

bool IsVersionGreater(const std::wstring& latest, const std::wstring& current) {
    std::vector<int> a = VersionParts(latest);
    std::vector<int> b = VersionParts(current);
    const size_t count = std::max(a.size(), b.size());
    a.resize(count);
    b.resize(count);
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i]) {
            return a[i] > b[i];
        }
    }
    return false;
}

UpdateResult FetchLatestRelease() {
    UpdateResult result{};
    HINTERNET session = WinHttpOpen(L"BlueGauge/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        result.message = L"无法初始化网络请求。";
        return result;
    }
    HINTERNET connect = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        result.message = L"无法连接 GitHub。";
        return result;
    }
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", L"/repos/HackenLeung/BlueGauge/releases/latest",
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        result.message = L"无法创建更新请求。";
        return result;
    }
    WinHttpAddRequestHeaders(request, L"User-Agent: BlueGauge\r\nAccept: application/vnd.github+json\r\n",
        static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(request, nullptr)) {
        result.message = L"检查更新失败，请稍后重试。";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return result;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (statusCode == 404) {
        result.status = UpdateNoRelease;
        result.htmlUrl = kProjectUrl;
        result.message = L"当前仓库还没有 GitHub Releases。";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return result;
    }
    if (statusCode != 200) {
        result.status = UpdateFailed;
        result.message = L"GitHub 返回状态码 " + std::to_wstring(statusCode) + L"。";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return result;
    }

    std::string body;
    DWORD available = 0;
    while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0) {
            break;
        }
        chunk.resize(read);
        body += chunk;
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    const auto tag = FindJsonString(body, "tag_name");
    const auto url = FindJsonString(body, "html_url");
    if (!tag.has_value()) {
        result.status = UpdateFailed;
        result.message = L"无法解析 GitHub Release 信息。";
        return result;
    }
    result.latestVersion = FromUtf8(*tag);
    result.htmlUrl = url.has_value() ? FromUtf8(*url) : kProjectUrl;
    if (IsVersionGreater(result.latestVersion, kAppVersion)) {
        result.status = UpdateAvailable;
        result.message = L"发现新版本 " + result.latestVersion + L"。";
    } else {
        result.status = UpdateLatest;
        result.message = L"已是最新版本。";
    }
    return result;
}
}

bool UpdateService::CheckAsync(HWND target) {
    bool expected = false;
    if (!checking_.compare_exchange_strong(expected, true)) {
        Logger::Instance().Warn(L"上一轮更新检查未结束，跳过本次请求");
        return false;
    }
    if (updateThread_.joinable()) {
        updateThread_.join();
    }
    updateThread_ = std::thread([target]() {
        auto result = std::make_unique<UpdateResult>(FetchLatestRelease());
        PostMessageW(target, WM_UPDATE_RESULT, 0, reinterpret_cast<LPARAM>(result.release()));
    });
    return true;
}

void UpdateService::CompleteCheck() {
    checking_ = false;
    if (updateThread_.joinable()) {
        updateThread_.join();
    }
}

void UpdateService::Shutdown() {
    if (updateThread_.joinable()) {
        updateThread_.join();
    }
}
