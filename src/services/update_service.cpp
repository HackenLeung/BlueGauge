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
constexpr wchar_t kGitHubHost[] = L"github.com";
constexpr wchar_t kGitHubApiHost[] = L"api.github.com";
constexpr wchar_t kLatestReleasePath[] = L"/HackenLeung/BlueGauge/releases/latest";
constexpr wchar_t kLatestReleaseApiPath[] = L"/repos/HackenLeung/BlueGauge/releases/latest";
constexpr wchar_t kReleaseTagMarker[] = L"/releases/tag/";

struct ReleaseInfo {
    std::wstring version;
    std::wstring htmlUrl;
};

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

std::wstring QueryHeaderString(HINTERNET request, DWORD infoLevel) {
    DWORD size = 0;
    if (WinHttpQueryHeaders(request, infoLevel, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size,
        WINHTTP_NO_HEADER_INDEX) || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return {};
    }

    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, infoLevel, WINHTTP_HEADER_NAME_BY_INDEX, value.data(), &size,
        WINHTTP_NO_HEADER_INDEX)) {
        return {};
    }
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

std::string ReadResponseBody(HINTERNET request) {
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
    return body;
}

std::optional<std::wstring> ExtractTagFromReleaseUrl(std::wstring url) {
    const size_t marker = url.find(kReleaseTagMarker);
    if (marker == std::wstring::npos) {
        return std::nullopt;
    }

    const size_t start = marker + wcslen(kReleaseTagMarker);
    const size_t end = url.find_first_of(L"?#", start);
    std::wstring tag = url.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
    if (tag.empty()) {
        return std::nullopt;
    }
    return tag;
}

std::optional<ReleaseInfo> FetchLatestReleaseRedirect(std::wstring& errorMessage, bool& noRelease) {
    HINTERNET session = WinHttpOpen(L"BlueGauge/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        errorMessage = L"无法初始化网络请求。";
        return std::nullopt;
    }
    HINTERNET connect = WinHttpConnect(session, kGitHubHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        errorMessage = L"无法连接 GitHub。";
        return std::nullopt;
    }
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", kLatestReleasePath,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        errorMessage = L"无法创建更新请求。";
        return std::nullopt;
    }

    DWORD disableRedirects = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &disableRedirects, sizeof(disableRedirects));
    WinHttpAddRequestHeaders(request, L"User-Agent: BlueGauge\r\nAccept: text/html,*/*\r\n",
        static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(request, nullptr)) {
        errorMessage = L"检查更新失败，请稍后重试。";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);

    if (statusCode == 404) {
        noRelease = true;
        errorMessage = L"当前仓库还没有 GitHub Releases。";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    if (statusCode < 300 || statusCode >= 400) {
        errorMessage = L"GitHub 返回状态码 " + std::to_wstring(statusCode) + L"。";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    std::wstring location = QueryHeaderString(request, WINHTTP_QUERY_LOCATION);
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (location.empty()) {
        errorMessage = L"无法解析 GitHub Release 跳转信息。";
        return std::nullopt;
    }
    if (location[0] == L'/') {
        location = std::wstring(L"https://github.com") + location;
    }

    const auto tag = ExtractTagFromReleaseUrl(location);
    if (!tag.has_value()) {
        errorMessage = L"无法解析 GitHub Release 版本。";
        return std::nullopt;
    }
    return ReleaseInfo{ *tag, location };
}

std::optional<ReleaseInfo> FetchLatestReleaseApi(std::wstring& errorMessage, bool& noRelease) {
    HINTERNET session = WinHttpOpen(L"BlueGauge/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        errorMessage = L"无法初始化网络请求。";
        return std::nullopt;
    }
    HINTERNET connect = WinHttpConnect(session, kGitHubApiHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        WinHttpCloseHandle(session);
        errorMessage = L"无法连接 GitHub。";
        return std::nullopt;
    }
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", kLatestReleaseApiPath,
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        errorMessage = L"无法创建更新请求。";
        return std::nullopt;
    }
    WinHttpAddRequestHeaders(request, L"User-Agent: BlueGauge\r\nAccept: application/vnd.github+json\r\n",
        static_cast<DWORD>(-1), WINHTTP_ADDREQ_FLAG_ADD);

    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
        || !WinHttpReceiveResponse(request, nullptr)) {
        errorMessage = L"检查更新失败，请稍后重试。";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    DWORD statusCode = 0;
    DWORD statusSize = sizeof(statusCode);
    WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSize, WINHTTP_NO_HEADER_INDEX);
    if (statusCode == 404) {
        noRelease = true;
        errorMessage = L"当前仓库还没有 GitHub Releases。";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }
    if (statusCode != 200) {
        errorMessage = L"GitHub API 返回状态码 " + std::to_wstring(statusCode) + L"。";
        WinHttpCloseHandle(request);
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    const std::string body = ReadResponseBody(request);
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    const auto tag = FindJsonString(body, "tag_name");
    const auto url = FindJsonString(body, "html_url");
    if (!tag.has_value()) {
        errorMessage = L"无法解析 GitHub Release 信息。";
        return std::nullopt;
    }
    return ReleaseInfo{ FromUtf8(*tag), url.has_value() ? FromUtf8(*url) : kProjectUrl };
}

UpdateResult BuildUpdateResult(const ReleaseInfo& release) {
    UpdateResult result{};
    result.latestVersion = release.version;
    result.htmlUrl = release.htmlUrl.empty() ? kProjectUrl : release.htmlUrl;
    if (IsVersionGreater(result.latestVersion, kAppVersion)) {
        result.status = UpdateAvailable;
        result.message = L"发现新版本 " + result.latestVersion + L"。";
    } else {
        result.status = UpdateLatest;
        result.message = L"已是最新版本。";
    }
    return result;
}

UpdateResult FetchLatestRelease() {
    std::wstring errorMessage;
    bool noRelease = false;
    if (const auto release = FetchLatestReleaseRedirect(errorMessage, noRelease)) {
        return BuildUpdateResult(*release);
    }
    if (!noRelease) {
        if (const auto release = FetchLatestReleaseApi(errorMessage, noRelease)) {
            return BuildUpdateResult(*release);
        }
    }

    UpdateResult result{};
    if (noRelease) {
        result.status = UpdateNoRelease;
        result.htmlUrl = kProjectUrl;
        result.message = L"当前仓库还没有 GitHub Releases。";
    } else {
        result.status = UpdateFailed;
        result.message = errorMessage.empty() ? L"检查更新失败，请稍后重试。" : errorMessage;
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
