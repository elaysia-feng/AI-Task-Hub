#pragma once

#include <string>

namespace update_checker {

// GitHub 上的一条 Win32 发行记录
struct ReleaseInfo {
    std::wstring tag;          // win32-v0.3.6
    std::wstring name;         // AI Task Hub Win32 v0.3.6
    std::wstring htmlUrl;      // 发行页地址
    std::wstring downloadUrl;  // 便携版 ZIP 直链（没有资产时为空）
    std::wstring publishedAt;  // ISO 时间
    bool prerelease = false;
};

// 一次检查的结果
struct UpdateResult {
    bool ok = false;           // 是否成功拿到发行信息
    bool upToDate = false;     // 当前版本是否已是最新
    std::wstring currentVersion;
    std::wstring latestTag;
    std::wstring latestName;
    std::wstring releaseUrl;
    std::wstring downloadUrl;
    std::wstring publishedAt;
    std::wstring error;        // 失败原因（中文，可直接展示）
};

// ---- 纯函数（可离线测试） ----

// 从标签或版本串里取出纯数字版本：win32-v0.3.6 → 0.3.6；0.3.6-win32 → 0.3.6
std::wstring extractVersionNumber(const std::wstring &tag);

// 逐段按数字比较版本：a<b 返回 -1，a==b 返回 0，a>b 返回 1（0.3.10 > 0.3.9）
int compareVersions(const std::wstring &a, const std::wstring &b);

// 从 GitHub releases 列表 JSON 里挑出最新的 Win32 发行（跳过 draft / prerelease / 非 win32 标签）
bool pickLatestWin32Release(const std::string &releasesJson, ReleaseInfo &out, std::wstring &error);

// ---- 联网检查 ----

// 阻塞调用，必须放到后台线程；失败时 result.ok=false 且 result.error 有中文说明
UpdateResult fetchLatestRelease(const std::wstring &currentVersion);

}  // namespace update_checker
