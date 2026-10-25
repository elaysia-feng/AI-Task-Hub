// Win32 版更新检查：查询 GitHub 最新 Win32 发行版。
//
// 与 Electron 版的 electron-updater 不同，这里只做「查询 + 对比」，不自动下载：
// 便携版是绿色 ZIP，下载与替换由用户自己决定，程序只负责告诉用户
// 「当前版本 / 最新发行版 / 是否有新版 / 去哪儿下」。
//
// 网络层用 Windows 自带的 WinHTTP（系统 TLS 证书库，免去第三方依赖）；
// 解析层复用仓库里的 json_lite，解析逻辑拆成纯函数，便于离线回归测试。

#include "update_checker.h"

#include "app_version.h"
#include "json_lite.h"

#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <string>
#include <vector>

namespace update_checker {
namespace {

constexpr DWORD kResolveTimeoutMs = 5000;
constexpr DWORD kConnectTimeoutMs = 5000;
constexpr DWORD kSendTimeoutMs = 5000;
constexpr DWORD kReceiveTimeoutMs = 8000;
constexpr size_t kMaxBodyBytes = 4 * 1024 * 1024;

std::wstring fromUtf8(const std::string &value) {
    return jsonlite::fromUtf8(value);
}

// 把 Windows 错误码翻译成用户看得懂的中文提示
std::wstring describeWin32Error(DWORD code) {
    switch (code) {
    case 12007:
        return L"无法解析 api.github.com，请检查 DNS 或网络";
    case 12029:
        return L"连接被拒绝，请检查网络或代理设置";
    case 12109:
    case 12110:
        return L"连接超时，请稍后重试";
    case 12160:
        return L"TLS 握手失败，请检查系统时间或证书";
    case 10060:
        return L"网络超时，请稍后重试";
    default:
        break;
    }
    wchar_t buffer[64]{};
    swprintf_s(buffer, L"网络请求失败（Windows 错误 %lu）", static_cast<unsigned long>(code));
    return buffer;
}

// 读取一条发行记录里的便携版 ZIP 直链
std::wstring pickZipAsset(const jsonlite::Value &release) {
    const jsonlite::Value *assets = release.get(L"assets");
    if (!assets || !assets->isArray()) return {};
    for (const auto &asset : assets->array()) {
        if (!asset.isObject()) continue;
        const jsonlite::Value *name = asset.get(L"name");
        const jsonlite::Value *url = asset.get(L"browser_download_url");
        if (!name || !name->isString() || !url || !url->isString()) continue;
        const std::wstring fileName = name->string();
        if (fileName.size() < 4) continue;
        if (fileName.compare(fileName.size() - 4, 4, L".zip") != 0) continue;
        return url->string();
    }
    return {};
}

// 从一条发行记录（GitHub release 对象）提取需要的信息
bool releaseFromObject(const jsonlite::Value &release, ReleaseInfo &out, std::wstring &error) {
    if (!release.isObject()) {
        error = L"发行数据格式异常";
        return false;
    }
    const jsonlite::Value *tag = release.get(L"tag_name");
    if (!tag || !tag->isString() || tag->string().empty()) {
        error = L"发行数据缺少 tag_name";
        return false;
    }
    out.tag = tag->string();
    const jsonlite::Value *name = release.get(L"name");
    out.name = name && name->isString() ? name->string() : out.tag;
    const jsonlite::Value *url = release.get(L"html_url");
    out.htmlUrl = url && url->isString() ? url->string() : L"";
    const jsonlite::Value *published = release.get(L"published_at");
    out.publishedAt = published && published->isString() ? published->string() : L"";
    const jsonlite::Value *prerelease = release.get(L"prerelease");
    out.prerelease = prerelease && prerelease->isBool() && prerelease->boolean();
    out.downloadUrl = pickZipAsset(release);
    return true;
}

// 对单个 release 对象做 Win32 过滤（标签前缀 + 非草稿 + 非预发布）
bool isWin32Release(const jsonlite::Value &release) {
    if (!release.isObject()) return false;
    const jsonlite::Value *draft = release.get(L"draft");
    if (draft && draft->isBool() && draft->boolean()) return false;
    const jsonlite::Value *prerelease = release.get(L"prerelease");
    if (prerelease && prerelease->isBool() && prerelease->boolean()) return false;
    const jsonlite::Value *tag = release.get(L"tag_name");
    if (!tag || !tag->isString()) return false;
    const std::wstring tagText = tag->string();
    const std::wstring prefix = kReleaseTagPrefix;
    return tagText.size() > prefix.size() &&
           tagText.compare(0, prefix.size(), prefix) == 0;
}

// 用 WinHTTP 发一次 GET，返回响应体
bool httpGet(const std::wstring &path, std::string &body, std::wstring &error) {
    HINTERNET session = WinHttpOpen(L"AI-Task-Hub-Win32", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) {
        error = describeWin32Error(GetLastError());
        return false;
    }
    // 显式要求 TLS 1.2/1.3：老系统默认可能仍允许更弱的协议
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
    protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
    WinHttpSetTimeouts(session, kResolveTimeoutMs, kConnectTimeoutMs, kSendTimeoutMs, kReceiveTimeoutMs);

    HINTERNET connect = WinHttpConnect(session, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) {
        error = describeWin32Error(GetLastError());
        WinHttpCloseHandle(session);
        return false;
    }
    HINTERNET request = WinHttpOpenRequest(connect, L"GET", path.c_str(), nullptr,
                                            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                            WINHTTP_FLAG_SECURE);
    if (!request) {
        error = describeWin32Error(GetLastError());
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return false;
    }
    // GitHub API 拒绝没有 User-Agent 的请求：优先用选项设置，
    // MinGW 的 WinHttpSetOption 只有 4 个参数（等价于 SETPARAM）。
    const wchar_t userAgent[] = L"AI-Task-Hub-Win32";
    std::wstring headers = L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n";
    LPVOID agentBuffer = const_cast<LPVOID>(static_cast<const void *>(userAgent));
    bool agentSet = WinHttpSetOption(request, WINHTTP_OPTION_USER_AGENT, agentBuffer, static_cast<DWORD>(-1)) != 0;
    if (!agentSet) {
        agentSet = WinHttpSetOption(request, WINHTTP_OPTION_USER_AGENT, agentBuffer,
                                     static_cast<DWORD>(wcslen(userAgent))) != 0;
    }
    if (!agentSet) {
        headers = L"User-Agent: AI-Task-Hub-Win32\r\n" + headers;
    }

    bool ok = false;
    if (WinHttpSendRequest(request, headers.c_str(), static_cast<DWORD>(-1), WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
        WinHttpReceiveResponse(request, nullptr)) {
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available > 0) {
            std::vector<char> buffer(available);
            DWORD read = 0;
            if (!WinHttpReadData(request, buffer.data(), available, &read)) break;
            if (read > 0) body.append(buffer.data(), read);
            if (body.size() > kMaxBodyBytes) break;
        }
        ok = true;
    } else {
        error = describeWin32Error(GetLastError());
    }
    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    if (!ok && error.empty()) error = L"网络请求失败";
    return ok;
}

}  // namespace

std::wstring extractVersionNumber(const std::wstring &tag) {
    // 标签里可能有多段数字（win32-v0.3.6 的 "32"、0.3.6-win32 的 "32"），
    // 只认「数字 + 点 + 数字」这种真正的版本号形态，取最后一次出现。
    std::wstring best;
    const size_t length = tag.size();
    for (size_t i = 0; i < length; ++i) {
        if (tag[i] < L'0' || tag[i] > L'9') continue;
        size_t cursor = i;
        while (cursor < length && tag[cursor] >= L'0' && tag[cursor] <= L'9') ++cursor;
        std::wstring run = tag.substr(i, cursor - i);
        size_t after = cursor;
        while (after < length && tag[after] == L'.') {
            size_t next = after + 1;
            while (next < length && tag[next] >= L'0' && tag[next] <= L'9') ++next;
            if (next == after + 1) break;  // 点后没有数字，版本号到此为止
            run += tag.substr(after, next - after);
            after = next;
        }
        if (run.find(L'.') != std::wstring::npos) best = run;
        i = after > cursor ? after - 1 : cursor - 1;
    }
    return best;
}

int compareVersions(const std::wstring &a, const std::wstring &b) {
    const auto split = [](const std::wstring &value) {
        std::vector<int> parts;
        std::wstring current;
        const auto flush = [&]() {
            parts.push_back(current.empty() ? 0 : _wtoi(current.c_str()));
            current.clear();
        };
        for (wchar_t ch : value) {
            if (ch >= L'0' && ch <= L'9') {
                current.push_back(ch);
            } else if (ch == L'.') {
                flush();
            } else {
                flush();
            }
        }
        flush();
        return parts;
    };
    const std::vector<int> left = split(a);
    const std::vector<int> right = split(b);
    const size_t count = std::max(left.size(), right.size());
    for (size_t i = 0; i < count; ++i) {
        const int x = i < left.size() ? left[i] : 0;
        const int y = i < right.size() ? right[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

bool pickLatestWin32Release(const std::string &releasesJson, ReleaseInfo &out, std::wstring &error) {
    jsonlite::Value parsed;
    std::string parseError;
    if (!jsonlite::parseUtf8(releasesJson, parsed, parseError)) {
        error = L"无法解析 GitHub 返回的发行数据";
        return false;
    }
    if (!parsed.isArray()) {
        error = L"GitHub 返回的发行列表格式异常";
        return false;
    }
    bool found = false;
    std::wstring bestVersion;
    for (const auto &entry : parsed.array()) {
        if (!isWin32Release(entry)) continue;
        ReleaseInfo candidate;
        std::wstring candidateError;
        if (!releaseFromObject(entry, candidate, candidateError)) continue;
        const std::wstring version = extractVersionNumber(candidate.tag);
        if (!found || compareVersions(version, bestVersion) > 0) {
            found = true;
            bestVersion = version;
            out = candidate;
        }
    }
    if (!found) {
        error = L"未找到 Win32 发行记录";
        return false;
    }
    return true;
}

UpdateResult fetchLatestRelease(const std::wstring &currentVersion) {
    UpdateResult result;
    result.currentVersion = currentVersion;

    const std::wstring prefix = kReleaseTagPrefix;
    std::wstring owner = kGitHubOwner;
    std::wstring repo = kGitHubRepo;

    // 1) 先试 /releases/latest：多数情况下最新发行就是 Win32 版，一次小请求即可
    std::string body;
    std::wstring error;
    ReleaseInfo release;
    if (httpGet(L"/repos/" + owner + L"/" + repo + L"/releases/latest", body, error)) {
        jsonlite::Value parsed;
        std::string parseError;
        if (jsonlite::parseUtf8(body, parsed, parseError) && isWin32Release(parsed)) {
            std::wstring releaseError;
            if (releaseFromObject(parsed, release, releaseError)) {
                result.ok = true;
                result.latestTag = release.tag;
                result.latestName = release.name;
                result.releaseUrl = release.htmlUrl;
                result.downloadUrl = release.downloadUrl;
                result.publishedAt = release.publishedAt;
                result.upToDate = compareVersions(extractVersionNumber(release.tag),
                                                 extractVersionNumber(currentVersion)) <= 0;
                return result;
            }
        }
    }

    // 2) 最新发行不是 Win32 版（例如刚发了 Electron 的 v* 标签）：拉列表按前缀过滤
    body.clear();
    if (!httpGet(L"/repos/" + owner + L"/" + repo + L"/releases?per_page=20", body, error)) {
        result.error = error.empty() ? L"检查更新失败" : error;
        return result;
    }
    std::wstring pickError;
    if (!pickLatestWin32Release(body, release, pickError)) {
        result.error = pickError.empty() ? L"未找到可用的 Win32 发行记录" : pickError;
        return result;
    }
    result.ok = true;
    result.latestTag = release.tag;
    result.latestName = release.name;
    result.releaseUrl = release.htmlUrl;
    result.downloadUrl = release.downloadUrl;
    result.publishedAt = release.publishedAt;
    result.upToDate = compareVersions(extractVersionNumber(release.tag),
                                      extractVersionNumber(currentVersion)) <= 0;
    return result;
}

}  // namespace update_checker
