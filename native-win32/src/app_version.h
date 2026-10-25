#pragma once

// 应用版本与发行源信息。
// 版本号只在这里维护一处：HTTP 接口的 version 字段、设置页「更新」页签、
// 更新检查的当前版本都用它，改版时不需要满仓库找常量。

// 对外展示的完整版本（带平台后缀）
constexpr wchar_t kAppVersion[] = L"0.3.6-win32";
// 与发行标签一致的纯数字版本（0.3.6-win32 → 0.3.6）
constexpr wchar_t kAppVersionNumber[] = L"0.3.6";

// 发行源：GitHub 公开仓库，无需 token 即可查询
constexpr wchar_t kGitHubOwner[] = L"elaysia-feng";
constexpr wchar_t kGitHubRepo[] = L"AI-Task-Hub";
// Win32 便携版使用独立标签前缀，与 Electron 的 v* 发行流互不干扰：
// /releases/latest 可能返回 Electron 的发行，必须按前缀过滤。
constexpr wchar_t kReleaseTagPrefix[] = L"win32-v";
