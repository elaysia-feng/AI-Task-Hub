#include "http_server.h"
#include "integration_manager.h"
#include "task_store.h"

#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kWindowClass[] = L"AI_TASK_HUB_WIN32_WINDOW";
constexpr UINT kRefreshMessage = WM_APP + 10;
constexpr UINT kTrayMessage = WM_APP + 11;
constexpr UINT_PTR kCollapseTimer = 42;
constexpr UINT_PTR kRefreshTimer = 43;
constexpr UINT_PTR kStartupActivateTimer = 44;
constexpr int kOrbSize = 52;
constexpr int kOrbPanelWidth = 240;
constexpr int kOrbPanelHeight = 360;
constexpr int kOrbPanelInset = 10;
// 与旧版截图的 1004×644 内容比例保持一致，启动时给用户一个稳定的宽屏工作区。
constexpr int kPanelWidth = 1004;
constexpr int kPanelHeight = 644;
constexpr wchar_t kApplicationRegistryKey[] = L"Software\\AI Task Hub";
constexpr wchar_t kRunRegistryKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"AI Task Hub Win32";

std::wstring modulePath() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1) return std::wstring(buffer.data(), length);
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring readRegistryString(const wchar_t *subKey, const wchar_t *valueName) {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, subKey, valueName, RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS || size < sizeof(wchar_t)) return {};
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, subKey, valueName, RRF_RT_REG_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS) return {};
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

bool writeRegistryString(const wchar_t *subKey, const wchar_t *valueName, const std::wstring &value, std::wstring &error) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, subKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        error = L"无法写入当前用户设置。";
        return false;
    }
    const LSTATUS result = RegSetValueExW(key, valueName, 0, REG_SZ,
        reinterpret_cast<const BYTE *>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    if (result == ERROR_SUCCESS) return true;
    error = L"保存当前用户设置失败（Windows 错误 " + std::to_wstring(result) + L"）。";
    return false;
}

bool autoStartEnabled() {
    const std::wstring expected = L"\"" + modulePath() + L"\"";
    return CompareStringOrdinal(readRegistryString(kRunRegistryKey, kRunValueName).c_str(), -1,
                                expected.c_str(), -1, TRUE) == CSTR_EQUAL;
}

bool setAutoStartEnabled(bool enabled, std::wstring &error) {
    if (enabled) return writeRegistryString(kRunRegistryKey, kRunValueName, L"\"" + modulePath() + L"\"", error);
    HKEY key = nullptr;
    const LSTATUS opened = RegOpenKeyExW(HKEY_CURRENT_USER, kRunRegistryKey, 0, KEY_SET_VALUE, &key);
    if (opened == ERROR_FILE_NOT_FOUND) return true;
    if (opened != ERROR_SUCCESS) { error = L"无法打开开机启动设置。"; return false; }
    const LSTATUS removed = RegDeleteValueW(key, kRunValueName);
    RegCloseKey(key);
    if (removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND) return true;
    error = L"关闭开机启动失败（Windows 错误 " + std::to_wstring(removed) + L"）。";
    return false;
}

std::wstring chooseDirectory(HWND owner) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    dialog->SetTitle(L"选择 AI Task Hub 数据库存储目录");
    if (FAILED(dialog->Show(owner))) return {};
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return {};
    PWSTR path = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || !path) return {};
    const std::wstring result(path);
    CoTaskMemFree(path);
    return result;
}

// 离屏 DIB + ID2D1DCRenderTarget + UpdateLayeredWindow 渲染管线。
// DIB 的 biHeight 取负数表示自顶向下、与 D2D1 默认像素布局一致，
// alpha 通道由 BLENDFUNCTION.AC_SRC_ALPHA 透传到桌面。
struct MemoryBacking {
    HBITMAP dib = nullptr;
    void *bits = nullptr;
    int width = 0;
    int height = 0;
    HDC memDC = nullptr;
    HBITMAP oldBitmap = nullptr;
    ComPtr<ID2D1DCRenderTarget> target;
};

struct HitRegion {
    RECT rect{};
    int id = 0;
    std::int64_t taskId = 0;
};

struct ThemeInfo {
    const wchar_t *id;
    const wchar_t *name;
};

constexpr std::array<ThemeInfo, 14> kThemes{{
    {L"default", L"AI 看板娘"}, {L"rei-ayanami", L"绫波丽"}, {L"tomo-ebizuka", L"海老塚智"},
    {L"elaina", L"伊蕾娜"}, {L"mutsumi-wakaba", L"若叶睦"}, {L"sakiko-togawa", L"丰川祥子"},
    {L"yui-hirasawa", L"平泽唯"}, {L"mio-akiyama", L"秋山澪"}, {L"ritsu-tainaka", L"田井中律"},
    {L"tsumugi-kotobuki", L"琴吹紬"}, {L"azusa-nakano", L"中野梓"},
    {L"ayaka-kamisato", L"神里绫华"}, {L"aemeath", L"爱弥斯"}, {L"shorekeeper", L"守岸人"},
}};

// 设计令牌：对齐 QML readonly property 数值。所有 RGB 在 #RRGGBB 位上；
// 透明通道按 0xAA 形式存到 ARGB 高位，colorFromArgb 解析后调用 PREMULTIPLIED 转换。
struct Palette {
    D2D1_COLOR_F canvas{};
    D2D1_COLOR_F sidebar{};
    D2D1_COLOR_F card{};
    D2D1_COLOR_F cardHover{};
    D2D1_COLOR_F tab{};
    D2D1_COLOR_F tabActive{};
    D2D1_COLOR_F border{};
    D2D1_COLOR_F borderStrong{};
    D2D1_COLOR_F textPrimary{};
    D2D1_COLOR_F textSecondary{};
    D2D1_COLOR_F muted{};
    D2D1_COLOR_F accent{};
    D2D1_COLOR_F accentSoft{};
    D2D1_COLOR_F accentLine{};
    D2D1_COLOR_F titlebarTop{};
    D2D1_COLOR_F titlebarBottom{};
    D2D1_COLOR_F success{};
    D2D1_COLOR_F warning{};
    D2D1_COLOR_F danger{};
    D2D1_COLOR_F dangerHover{};
};

D2D1_COLOR_F colorFromArgb(unsigned int argb) {
    const float a = static_cast<float>((argb >> 24) & 0xff) / 255.0f;
    const float r = static_cast<float>((argb >> 16) & 0xff) / 255.0f;
    const float g = static_cast<float>((argb >> 8) & 0xff) / 255.0f;
    const float b = static_cast<float>(argb & 0xff) / 255.0f;
    // D2D1_COLOR_F 传入的是 straight RGBA；RenderTarget 会按其像素格式统一预乘。
    // 这里不能提前乘一次，否则半透明卡片会被二次预乘而明显发黑。
    return D2D1::ColorF(r, g, b, a);
}

D2D1_COLOR_F color(unsigned int rgb, float alpha = 1.0f) {
    return D2D1::ColorF(static_cast<float>((rgb >> 16) & 0xff) / 255.0f,
                        static_cast<float>((rgb >> 8) & 0xff) / 255.0f,
                        static_cast<float>(rgb & 0xff) / 255.0f, alpha);
}

D2D1_COLOR_F blend(D2D1_COLOR_F value, float alpha) {
    value.a = alpha;
    return value;
}

const Palette kPaletteDark = {
    colorFromArgb(0xff111416),   // canvas
    colorFromArgb(0x99080a0e),   // sidebar
    colorFromArgb(0x7c10121a),   // card
    colorFromArgb(0x9c161a24),   // cardHover
    colorFromArgb(0x5010121a),   // tab
    colorFromArgb(0x70161a24),   // tabActive
    colorFromArgb(0x14ffffff),   // border
    colorFromArgb(0x24ffffff),   // borderStrong
    colorFromArgb(0xfff1f0ec),   // textPrimary
    colorFromArgb(0xffaaa9a3),   // textSecondary
    colorFromArgb(0xff969994),   // muted
    colorFromArgb(0xffdf7654),   // accent
    colorFromArgb(0x24df7654),   // accentSoft
    colorFromArgb(0x66df7654),   // accentLine
    colorFromArgb(0x55111416),   // titlebarTop
    colorFromArgb(0x00111416),   // titlebarBottom (transparent)
    colorFromArgb(0xff22c55e),   // success
    colorFromArgb(0xfff59e0b),   // warning
    colorFromArgb(0xffef4444),   // danger
    colorFromArgb(0xffe81123),   // dangerHover
};

const Palette kPaletteLight = {
    colorFromArgb(0xffeef1f5),   // canvas
    colorFromArgb(0xf2f6f7fa),   // sidebar
    colorFromArgb(0xeafafcff),   // card
    colorFromArgb(0xf5ffffff),   // cardHover
    colorFromArgb(0x88ffffff),   // tab
    colorFromArgb(0xa8ffffff),   // tabActive
    colorFromArgb(0x3a243044),   // border
    colorFromArgb(0x64212a3a),   // borderStrong
    colorFromArgb(0xff202631),   // textPrimary
    colorFromArgb(0xff4f5866),   // textSecondary
    colorFromArgb(0xff66707d),   // muted
    colorFromArgb(0xffc95f43),   // accent
    colorFromArgb(0x25c95f43),   // accentSoft
    colorFromArgb(0x70c95f43),   // accentLine
    colorFromArgb(0xd9eef1f5),   // titlebarTop
    colorFromArgb(0xe8eef1f5),   // titlebarBottom
    colorFromArgb(0xff22c55e),   // success
    colorFromArgb(0xfff59e0b),   // warning
    colorFromArgb(0xffef4444),   // danger
    colorFromArgb(0xffe81123),   // dangerHover
};

enum HitId {
    HitNone = 0,
    HitQueue = 1,
    HitHistory = 2,
    HitSettings = 3,
    HitOrb = 4,
    HitClose = 5,
    HitMinimize = 6,
    HitMaximize = 32,
    HitMarkAll = 7,
    HitClearView = 8,
    HitOpenSelected = 9,
    HitViewSelected = 10,
    HitDeleteSelected = 11,
    HitCloseDetail = 12,
    HitPickWallpaper = 13,
    HitPickIcon = 14,
    HitClearAppearance = 15,
    HitSearch = 16,
    HitStatusCycle = 17,
    HitThemeToggle = 18,
    HitEnterOrb = 19,
    HitStatusChip = 20,
    HitSourceChip = 21,
    HitCardIgnore = 22,
    HitCardOpen = 23,
    HitDetailRead = 24,
    HitDetailDelete = 25,
    HitDetailOpen = 26,
    HitSectionWallpaper = 27,
    HitSectionIcon = 28,
    HitSectionOpenDir = 29,
    HitSectionMarkRead = 30,
    HitSectionClearAll = 31,
    HitCleanupMenu = 33,
    HitOpacityDown = 34,
    HitOpacityUp = 35,
    HitBlurDown = 36,
    HitBlurUp = 37,
    HitIntegrationClaude = 38,
    HitIntegrationCodex = 39,
    HitIntegrationChatGpt = 40,
    HitIntegrationOpenDir = 41,
    HitResetFilters = 42,
    HitNotifications = 43,
    HitAutoStart = 44,
    HitChangeDataDirectory = 45,
    HitClearWallpaper = 46,
    HitClearIcon = 47,
    HitCleanupBackdrop = 60,
    HitCleanupQueue = 61,
    HitCleanupCompleted = 62,
    HitCleanupHistory = 63,
    HitCleanupSources = 64,
    HitCleanupAll = 65,
    HitCleanupSourceBase = 66,
    HitCleanupConfirmYes = 70,
    HitCleanupConfirmNo = 71,
    HitSettingsTabBase = 50,
    HitThemeBase = 100,
    HitIconBase = 200,
    HitSourceBase = 300,
    HitStatusBase = 400,
    HitCardBase = 1000,
};

RECT rectFrom(int left, int top, int right, int bottom) { return RECT{left, top, right, bottom}; }

std::wstring shorten(std::wstring value, size_t maxLength) {
    for (auto &ch : value) if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
    while (value.find(L"  ") != std::wstring::npos) value.replace(value.find(L"  "), 2, L" ");
    if (value.size() <= maxLength) return value;
    if (maxLength < 2) return value.substr(0, maxLength);
    return value.substr(0, maxLength - 1) + L"…";
}

std::wstring sourceLabel(const std::wstring &source) {
    if (source == L"CHATGPT") return L"GPT 网页";
    if (source == L"CLAUDE_CODE") return L"Claude Code";
    if (source == L"CODEX") return L"Codex";
    return L"其他";
}

D2D1_COLOR_F sourceColor(const std::wstring &source) {
    if (source == L"CHATGPT") return color(0x10a37f);
    if (source == L"CLAUDE_CODE") return color(0xd97757);
    if (source == L"CODEX") return color(0x4f8ff7);
    return color(0x8b5cf6);
}

std::wstring statusLabel(const std::wstring &status) {
    if (status == L"TASK_STARTED") return L"任务开始";
    if (status == L"TASK_COMPLETED") return L"任务完成";
    if (status == L"TASK_FAILED") return L"任务失败";
    if (status == L"TASK_NEEDS_INPUT") return L"等待输入";
    if (status == L"TASK_VIEWED") return L"已读";
    if (status == L"TASK_IGNORED") return L"已忽略";
    if (status == L"RUNNING") return L"执行中";
    if (status == L"NEEDS_INPUT") return L"等待输入";
    if (status == L"COMPLETED_UNREAD") return L"已完成";
    if (status == L"FAILED_UNREAD") return L"失败";
    if (status == L"VIEWED") return L"已查看";
    if (status == L"IGNORED") return L"已忽略";
    return status;
}

D2D1_COLOR_F statusColor(const std::wstring &status) {
    if (status == L"RUNNING") return color(0x38bdf8);
    if (status == L"NEEDS_INPUT") return color(0xf59e0b);
    if (status == L"COMPLETED_UNREAD") return color(0x22c55e);
    if (status == L"FAILED_UNREAD") return color(0xef4444);
    if (status == L"VIEWED") return color(0x8e98ab);
    return color(0x6d7485);
}

std::wstring formatTime(const std::wstring &value) {
    if (value.size() >= 16 && value[4] == L'-' && value[7] == L'-') {
        std::wstring result = value.substr(5, 11);
        if (result[5] == L'T') result[5] = L' ';
        return result;
    }
    return shorten(value, 18);
}

// 状态色条集中函数：和 QML 状态映射一致，亮/暗都按 status 字段取色。
D2D1_COLOR_F statusBarColor(const std::string &status) {
    const std::wstring w(status.begin(), status.end());
    return statusColor(w);
}

} // namespace

class Win32App final {
public:
    Win32App() : store_(), integrations_(), server_(store_, integrations_) {
        opacityPercent_ = store_.windowOpacityPercent();
        blurLevel_ = store_.backgroundBlurLevel();
        darkMode_ = store_.darkMode();
        notificationsEnabled_ = store_.notificationsEnabled();
        autoStartEnabled_ = autoStartEnabled();
    }
    ~Win32App() {
        if (hwnd_) Shell_NotifyIconW(NIM_DELETE, &tray_);
        server_.stop();
        destroyBacking();
    }

    int run(HINSTANCE instance) {
        instance_ = instance;
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (!registerWindowClass()) {
            showStartupError(L"注册窗口类失败");
            return 1;
        }
        store_.setChangeCallback(&Win32App::onStoreChanged, this);
        refreshSnapshot();
        refreshIntegrationStatus();
        if (!server_.start(17891)) serverWarning_ = L"HTTP 17891 已被其他进程占用";
        createWindow();
        if (!hwnd_) {
            showStartupError(L"创建主窗口失败");
            return 1;
        }
        initTray();
        // 默认以轻量悬浮球常驻右上角；点击小球仍可打开完整任务中心。
        enterOrbMode();
        ShowWindow(hwnd_, SW_SHOWNORMAL);
        UpdateWindow(hwnd_);
        // UpdateLayeredWindow 窗口首次显示时可能不会收到有效的 WM_PAINT；启动阶段主动提交首帧，
        // 避免窗口句柄可见但桌面仍透出到后面的 Terminal/Explorer。
        render();
        // 无边框悬浮窗口不一定会被系统自动激活；启动时显式置前，避免双击后“进程在但看不见”。
        activateAtStartup();
        // 从 Windows Terminal 启动时，首次 SetForegroundWindow 可能被前台锁定策略拒绝，
        // 延迟一帧再提升一次，确保用户看到的是主界面而不是终端窗口。
        SetTimer(hwnd_, kStartupActivateTimer, 180, nullptr);

        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        return static_cast<int>(message.wParam);
    }

private:
    void activateAtStartup() {
        if (!hwnd_) return;
        const HWND foreground = GetForegroundWindow();
        DWORD foregroundThread = 0;
        if (foreground) foregroundThread = GetWindowThreadProcessId(foreground, nullptr);
        const DWORD currentThread = GetCurrentThreadId();
        const bool attached = foregroundThread != 0 && foregroundThread != currentThread &&
                              AttachThreadInput(foregroundThread, currentThread, TRUE) != FALSE;
        RECT window{};
        GetWindowRect(hwnd_, &window);
        // 短暂置顶再恢复层级，解决前台锁定导致的 z-order 覆盖；悬浮球必须继续保持置顶。
        SetWindowPos(hwnd_, HWND_TOPMOST, window.left, window.top,
                     window.right - window.left, window.bottom - window.top,
                     SWP_SHOWWINDOW);
        BringWindowToTop(hwnd_);
        if (!orbMode_) {
            SetWindowPos(hwnd_, HWND_NOTOPMOST, window.left, window.top,
                         window.right - window.left, window.bottom - window.top,
                         SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        SetForegroundWindow(hwnd_);
        SetActiveWindow(hwnd_);
        if (attached) AttachThreadInput(foregroundThread, currentThread, FALSE);
    }

    void showStartupError(const wchar_t *reason) const {
        const DWORD code = GetLastError();
        std::wstring message = reason;
        message += L"\nWindows 错误码：";
        message += std::to_wstring(code);
        MessageBoxW(nullptr, message.c_str(), L"AI Task Hub Win32 启动失败", MB_OK | MB_ICONERROR);
    }

    static void onStoreChanged(void *context) {
        auto *app = static_cast<Win32App *>(context);
        if (app && app->hwnd_) PostMessageW(app->hwnd_, kRefreshMessage, 0, 0);
    }

    bool registerWindowClass() {
        WNDCLASSEXW klass{sizeof(WNDCLASSEXW)};
        klass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
        klass.lpfnWndProc = &Win32App::windowProc;
        klass.hInstance = instance_;
        klass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        klass.hbrBackground = nullptr;
        klass.lpszClassName = kWindowClass;
        klass.hIcon = loadAppIcon(32);
        klass.hIconSm = loadAppIcon(16);
        return RegisterClassExW(&klass) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }

    HICON loadAppIcon(int size) const {
        const std::wstring path = resourceDirectory() + L"\\icon.ico";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return static_cast<HICON>(LoadImageW(nullptr, path.c_str(), IMAGE_ICON, size, size, LR_LOADFROMFILE));
        }
        return LoadIconW(nullptr, IDI_APPLICATION);
    }

    void createWindow() {
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        const int x = work.left + (work.right - work.left - kPanelWidth) / 2;
        const int y = work.top + (work.bottom - work.top - kPanelHeight) / 2;
        // WS_EX_LAYERED + WS_EX_NOREDIRECTIONBITMAP 关闭 DWM 接管，使 UpdateLayeredWindow 生效。
        hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW,
                                kWindowClass, L"AI Task Hub",
                                WS_POPUP | WS_THICKFRAME | WS_MAXIMIZEBOX, x, y, kPanelWidth, kPanelHeight,
                                nullptr, nullptr, instance_, this);
        if (!hwnd_) return;
        SetWindowPos(hwnd_, HWND_TOP, x, y, kPanelWidth, kPanelHeight,
                     SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        SetTimer(hwnd_, kRefreshTimer, 2500, nullptr);
    }

    void initTray() {
        tray_.cbSize = sizeof(tray_);
        tray_.hWnd = hwnd_;
        tray_.uID = 1;
        tray_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
        tray_.uCallbackMessage = kTrayMessage;
        tray_.hIcon = loadAppIcon(32);
        wcscpy_s(tray_.szTip, L"AI Task Hub · Win32 原生");
        Shell_NotifyIconW(NIM_ADD, &tray_);
        tray_.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &tray_);
    }

    std::wstring resourceDirectory() const {
        wchar_t module[MAX_PATH]{};
        GetModuleFileNameW(nullptr, module, MAX_PATH);
        const std::filesystem::path appDir(module);
        const std::array<std::filesystem::path, 4> candidates{
            appDir.parent_path() / L"resources",
            appDir.parent_path().parent_path() / L"desktop" / L"resources",
            std::filesystem::current_path() / L"desktop" / L"resources",
            std::filesystem::path(L"F:\\myInterestingProgram\\AI-Task-Hub\\desktop\\resources")};
        for (const auto &candidate : candidates) {
            if (GetFileAttributesW((candidate / L"presets" / L"default.png").wstring().c_str()) != INVALID_FILE_ATTRIBUTES)
                return candidate.wstring();
        }
        return candidates.front().wstring();
    }

    std::wstring wallpaperFile() const {
        const std::wstring configured = store_.wallpaperPath();
        if (!configured.empty()) return configured;
        const wchar_t *variant = darkMode_ ? L"dark" : L"light";
        return resourceDirectory() + L"\\themes\\" + store_.themeId() + L"\\wallpaper-" + variant + L".png";
    }

    std::wstring avatarFile() const {
        const std::wstring configured = store_.userIconPath();
        if (!configured.empty()) return configured;
        return resourceDirectory() + L"\\presets\\" + store_.userIconPreset() + L".png";
    }

    // 工厂与文字格式只创建一次，缓存复用。
    bool ensureFactory() {
        if (d2dFactory_) return true;
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory),
                                     reinterpret_cast<void **>(d2dFactory_.GetAddressOf())))) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), &writeFactory_))) return false;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&wicFactory_)))) return false;
        return true;
    }

    // 离屏 BGRA DIB：top-down、bitfields mask，alpha 通道由 ULW_ALPHA 透出。
    bool rebuildBacking(int width, int height) {
        if (width <= 0 || height <= 0) return false;
        if (backing_.dib && backing_.width == width && backing_.height == height && backing_.target) return true;
        destroyBacking();
        BITMAPV5HEADER info{};
        info.bV5Size = sizeof(BITMAPV5HEADER);
        info.bV5Width = width;
        info.bV5Height = -height; // top-down DIB
        info.bV5Planes = 1;
        info.bV5BitCount = 32;
        info.bV5Compression = BI_BITFIELDS;
        info.bV5SizeImage = static_cast<DWORD>(width * height * 4);
        info.bV5RedMask = 0x00FF0000u;
        info.bV5GreenMask = 0x0000FF00u;
        info.bV5BlueMask = 0x000000FFu;
        info.bV5AlphaMask = 0xFF000000u;
        HDC screen = GetDC(nullptr);
        backing_.dib = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO *>(&info),
                                        DIB_RGB_COLORS, &backing_.bits, nullptr, 0);
        ReleaseDC(nullptr, screen);
        if (!backing_.dib || !backing_.bits) return false;
        backing_.width = width;
        backing_.height = height;
        backing_.memDC = CreateCompatibleDC(nullptr);
        if (!backing_.memDC) { destroyBacking(); return false; }
        backing_.oldBitmap = static_cast<HBITMAP>(SelectObject(backing_.memDC, backing_.dib));
        const D2D1_RENDER_TARGET_PROPERTIES rtProps = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_SOFTWARE,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
            96.0f, 96.0f);
        if (FAILED(d2dFactory_->CreateDCRenderTarget(&rtProps, &backing_.target))) {
            destroyBacking();
            return false;
        }
        // DIB 是预乘 alpha，第一帧清空到全 0 让 UpdateLayeredWindow 透出桌面。
        std::memset(backing_.bits, 0, static_cast<size_t>(info.bV5SizeImage));
        return true;
    }

    void destroyBacking() {
        // 位图和画刷属于创建它们的渲染目标，窗口切换尺寸后必须一起重建。
        brush_.Reset();
        wallpaper_.Reset();
        avatar_.Reset();
        loadedWallpaper_.clear();
        loadedAvatar_.clear();
        if (backing_.memDC) {
            if (backing_.oldBitmap) SelectObject(backing_.memDC, backing_.oldBitmap);
            DeleteDC(backing_.memDC);
        }
        backing_.memDC = nullptr;
        backing_.oldBitmap = nullptr;
        backing_.target.Reset();
        if (backing_.dib) DeleteObject(backing_.dib);
        backing_.dib = nullptr;
        backing_.bits = nullptr;
        backing_.width = 0;
        backing_.height = 0;
        previewBitmaps_.clear();
    }

    bool bindBacking() {
        if (!backing_.target || !backing_.memDC) return false;
        const RECT rect{0, 0, backing_.width, backing_.height};
        return SUCCEEDED(backing_.target->BindDC(backing_.memDC, &rect));
    }

    bool loadBitmap(const std::wstring &path, UINT maxSize, ComPtr<ID2D1Bitmap> &result) {
        if (!wicFactory_ || !backing_.target ||
            GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
        ComPtr<IWICBitmapDecoder> decoder;
        if (FAILED(wicFactory_->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                           WICDecodeMetadataCacheOnLoad, &decoder))) return false;
        ComPtr<IWICBitmapFrameDecode> frame;
        if (FAILED(decoder->GetFrame(0, &frame))) return false;
        UINT width = 0, height = 0;
        frame->GetSize(&width, &height);
        if (!width || !height) return false;
        if (maxSize == 0) {
            // 壁纸仅解码到覆盖当前窗口所需的像素尺寸，不常驻整张大图。
            const double fit = std::min(1.0, std::max(static_cast<double>(backing_.width) / width,
                                                     static_cast<double>(backing_.height) / height));
            maxSize = static_cast<UINT>(std::ceil(std::max(width, height) * fit));
        }
        ComPtr<IWICBitmapSource> source = frame;
        if (maxSize > 0 && (width > maxSize || height > maxSize)) {
            const double ratio = std::min(static_cast<double>(maxSize) / width, static_cast<double>(maxSize) / height);
            ComPtr<IWICBitmapScaler> scaler;
            if (FAILED(wicFactory_->CreateBitmapScaler(&scaler)) ||
                FAILED(scaler->Initialize(frame.Get(), static_cast<UINT>(width * ratio), static_cast<UINT>(height * ratio), WICBitmapInterpolationModeCubic))) return false;
            source = scaler;
            scaler->GetSize(&width, &height);
        }
        ComPtr<IWICFormatConverter> converter;
        if (FAILED(wicFactory_->CreateFormatConverter(&converter)) ||
            FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA,
                                          WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut))) return false;
        converter->GetSize(&width, &height);
        // 走 DCRenderTarget 的 CreateBitmap + CopyFromMemory 路径，
        // 不依赖任何 HWND 绑定的 RenderTarget。
        const D2D1_BITMAP_PROPERTIES props = D2D1::BitmapProperties(
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        const HRESULT hr = backing_.target->CreateBitmap(D2D1::SizeU(width, height), props, &result);
        if (FAILED(hr)) return false;
        std::vector<BYTE> pixels(static_cast<size_t>(width) * height * 4);
        if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(pixels.size()), pixels.data()))) {
            result.Reset();
            return false;
        }
        return SUCCEEDED(result->CopyFromMemory(nullptr, pixels.data(), width * 4));
    }

    ID2D1Bitmap *previewBitmap(const std::wstring &path) {
        const auto found = previewBitmaps_.find(path);
        if (found != previewBitmaps_.end()) return found->second.Get();
        ComPtr<ID2D1Bitmap> bitmap;
        if (!loadBitmap(path, 64, bitmap)) return nullptr;
        auto [it, inserted] = previewBitmaps_.emplace(path, std::move(bitmap));
        return inserted ? it->second.Get() : nullptr;
    }

    void reloadImages(bool force = false) {
        const std::wstring nextWallpaper = orbMode_ ? L"" : wallpaperFile();
        const std::wstring nextAvatar = avatarFile();
        if (!force && nextWallpaper == loadedWallpaper_ && nextAvatar == loadedAvatar_) return;
        loadedWallpaper_ = nextWallpaper;
        loadedAvatar_ = nextAvatar;
        wallpaper_.Reset();
        avatar_.Reset();
        if (backing_.target) {
            // 通过低分辨率重采样制造轻量的背景柔化效果，避免引入常驻 GPU/CPU 模糊管线。
            const UINT wallpaperSize = blurLevel_ == 0 ? 0u : (blurLevel_ == 1 ? 720u : 360u);
            if (!loadedWallpaper_.empty()) loadBitmap(loadedWallpaper_, wallpaperSize, wallpaper_);
            loadBitmap(loadedAvatar_, 256, avatar_);
        }
    }

    ComPtr<IDWriteTextFormat> textFormat(float size, bool bold, bool wrap, bool center = false) {
        const int key = static_cast<int>(size * 10) * 8 + (bold ? 4 : 0) + (wrap ? 2 : 0) + (center ? 1 : 0);
        const auto found = formats_.find(key);
        if (found != formats_.end()) return found->second;
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(writeFactory_->CreateTextFormat(L"Microsoft YaHei UI", nullptr,
                                                    bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                                    DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                    size, L"zh-CN", &format))) return nullptr;
        format->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        format->SetTextAlignment(center ? DWRITE_TEXT_ALIGNMENT_CENTER : DWRITE_TEXT_ALIGNMENT_LEADING);
        format->SetParagraphAlignment(center ? DWRITE_PARAGRAPH_ALIGNMENT_CENTER : DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        formats_[key] = format;
        return format;
    }

    void setBrush(D2D1_COLOR_F value) { brush_->SetColor(value); }
    void fillRect(float left, float top, float right, float bottom, D2D1_COLOR_F value) {
        setBrush(value);
        target()->FillRectangle(D2D1::RectF(left, top, right, bottom), brush_.Get());
    }
    void fillRound(float left, float top, float right, float bottom, float radius, D2D1_COLOR_F value) {
        setBrush(value);
        target()->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left, top, right, bottom), radius, radius), brush_.Get());
    }
    void strokeRound(float left, float top, float right, float bottom, float radius, float width, D2D1_COLOR_F value) {
        setBrush(value);
        target()->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left, top, right, bottom), radius, radius), brush_.Get(), width);
    }
    void line(float x1, float y1, float x2, float y2, float width, D2D1_COLOR_F value) {
        setBrush(value);
        target()->DrawLine(D2D1::Point2F(x1, y1), D2D1::Point2F(x2, y2), brush_.Get(), width);
    }
    void text(const std::wstring &value, float left, float top, float right, float bottom,
              float size, D2D1_COLOR_F valueColor, bool bold = false, bool wrap = false) {
        if (value.empty()) return;
        const auto format = textFormat(size, bold, wrap);
        if (!format) return;
        setBrush(valueColor);
        target()->DrawTextW(value.c_str(), static_cast<UINT32>(value.size()), format.Get(),
                            D2D1::RectF(left, top, right, bottom), brush_.Get(),
                            D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void textCentered(const std::wstring &value, float left, float top, float right, float bottom,
                      float size, D2D1_COLOR_F valueColor, bool bold = false) {
        if (value.empty()) return;
        const auto format = textFormat(size, bold, false, true);
        if (!format) return;
        setBrush(valueColor);
        target()->DrawTextW(value.c_str(), static_cast<UINT32>(value.size()), format.Get(),
                            D2D1::RectF(left, top, right, bottom), brush_.Get(),
                            D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    void circle(float x, float y, float radius, D2D1_COLOR_F value) {
        setBrush(value);
        target()->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius), brush_.Get());
    }

    int wrappedHeight(const std::wstring &value, float width, float size, bool bold = false) {
        if (value.empty()) return 0;
        const auto format = textFormat(size, bold, true);
        ComPtr<IDWriteTextLayout> layout;
        if (!format || FAILED(writeFactory_->CreateTextLayout(value.c_str(), static_cast<UINT32>(value.size()),
                format.Get(), width, 1000000, &layout))) return 24;
        DWRITE_TEXT_METRICS metrics{};
        layout->GetMetrics(&metrics);
        return static_cast<int>(std::ceil(metrics.height)) + 2;
    }
    void circleStroke(float x, float y, float radius, float width, D2D1_COLOR_F value) {
        setBrush(value);
        target()->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius), brush_.Get(), width);
    }

    ID2D1RenderTarget *target() const { return backing_.target.Get(); }

    void drawBitmapCircle(ID2D1Bitmap *bitmap, float x, float y, float diameter, float opacity = 1.0f) {
        if (!bitmap) {
            circle(x + diameter / 2, y + diameter / 2, diameter / 2, palette().cardHover);
            circleStroke(x + diameter / 2, y + diameter / 2, diameter / 2, 1.0f, palette().border);
            return;
        }
        ComPtr<ID2D1Layer> layer;
        target()->CreateLayer(nullptr, &layer);
        if (layer) {
            ComPtr<ID2D1EllipseGeometry> geometry;
            d2dFactory_->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(x + diameter / 2, y + diameter / 2), diameter / 2, diameter / 2), &geometry);
            if (geometry) {
                target()->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), geometry.Get()), layer.Get());
                target()->DrawBitmap(bitmap, D2D1::RectF(x, y, x + diameter, y + diameter), opacity,
                                     D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                target()->PopLayer();
            }
        }
        circleStroke(x + diameter / 2, y + diameter / 2, diameter / 2, 1.0f, palette().border);
    }

    void drawBitmapCover(ID2D1Bitmap *bitmap, float x, float y, float width, float height,
                         float opacity = 1.0f) {
        if (!bitmap) {
            fillRound(x, y, x + width, y + height, 8, palette().cardHover);
            strokeRound(x, y, x + width, y + height, 8, 1.0f, palette().border);
            return;
        }
        const D2D1_SIZE_F imageSize = bitmap->GetSize();
        if (imageSize.width <= 0 || imageSize.height <= 0) return;
        const float scale = std::max(width / imageSize.width, height / imageSize.height);
        const float drawWidth = imageSize.width * scale;
        const float drawHeight = imageSize.height * scale;
        const float offsetX = x + (width - drawWidth) / 2.0f;
        const float offsetY = y + (height - drawHeight) / 2.0f;
        target()->PushAxisAlignedClip(D2D1::RectF(x, y, x + width, y + height),
                                      D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        target()->DrawBitmap(bitmap, D2D1::RectF(offsetX, offsetY, offsetX + drawWidth, offsetY + drawHeight),
                             opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        target()->PopAxisAlignedClip();
        strokeRound(x, y, x + width, y + height, 8, 1.0f, palette().border);
    }

    void drawWallpaper(int width, int height) {
        if (wallpaper_) {
            // 按比例铺满窗口并裁剪边缘，避免 16:9 壁纸被拉伸成旧版面板比例。
            const D2D1_SIZE_F imageSize = wallpaper_->GetSize();
            const float scale = std::max(static_cast<float>(width) / imageSize.width,
                                         static_cast<float>(height) / imageSize.height);
            const float drawWidth = imageSize.width * scale;
            const float drawHeight = imageSize.height * scale;
            const float offsetX = (static_cast<float>(width) - drawWidth) / 2.0f;
            const float offsetY = (static_cast<float>(height) - drawHeight) / 2.0f;
            target()->DrawBitmap(wallpaper_.Get(),
                                  D2D1::RectF(offsetX, offsetY, offsetX + drawWidth, offsetY + drawHeight),
                                  1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        } else {
            // 没有壁纸：直接铺底色（dark/light canvas）让面板有底。
            fillRect(0, 0, static_cast<float>(width), static_cast<float>(height), palette().canvas);
        }
        // 全局轻压暗：在壁纸存在时模拟半透叠层；没有壁纸时不画这一层避免纯色死板。
        if (wallpaper_) {
            // 亮色壁纸本身已完成调色，只加极轻的遮罩保留背景细节。
            const D2D1_COLOR_F dim = darkMode_ ? colorFromArgb(0x52000000u) : colorFromArgb(0x18000000u);
            fillRect(0, 0, static_cast<float>(width), static_cast<float>(height), dim);
        }
    }

    void setHitClip(const RECT &rect) {
        hitClip_ = rect;
        hitClipEnabled_ = true;
    }

    void clearHitClip() { hitClipEnabled_ = false; }

    void addHit(int id, const RECT &rect, std::int64_t taskId = 0) {
        RECT clipped = rect;
        if (hitClipEnabled_) {
            clipped.left = std::max(clipped.left, hitClip_.left);
            clipped.top = std::max(clipped.top, hitClip_.top);
            clipped.right = std::min(clipped.right, hitClip_.right);
            clipped.bottom = std::min(clipped.bottom, hitClip_.bottom);
            if (clipped.left >= clipped.right || clipped.top >= clipped.bottom) return;
        }
        hits_.push_back(HitRegion{clipped, id, taskId});
    }
    int hitAt(POINT point) const {
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it) {
            if (PtInRect(&it->rect, point)) return it->id;
        }
        return HitNone;
    }

    std::int64_t taskAt(POINT point) const {
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
            if (PtInRect(&it->rect, point)) return it->taskId;
        return 0;
    }

    const Palette &palette() const { return darkMode_ ? kPaletteDark : kPaletteLight; }

    D2D1_COLOR_F taskCardSurface(bool elevated = false) const {
        const Palette &p = palette();
        if (darkMode_) return elevated ? p.cardHover : p.card;
        // 任务页和设置页都保持可见壁纸的玻璃层次；悬停/选中时只提高一点不透明度。
        return colorFromArgb(elevated ? 0x9cf9fcffu : 0x78f6f9ffu);
    }

    void toggleDarkMode() {
        darkMode_ = !darkMode_;
        store_.setDarkMode(darkMode_);
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void refreshSnapshot() {
        HubSnapshot next = store_.snapshot(200);
        if (snapshotInitialized_) {
            for (const auto &task : next.queue) {
                if (task.eventType != L"TASK_COMPLETED" && task.eventType != L"TASK_FAILED" &&
                    task.eventType != L"TASK_NEEDS_INPUT") continue;
                const HubTask *previous = previousTask(task.id);
                if (!previous || previous->eventType != task.eventType) notifyTaskChanged(task);
            }
        }
        snapshot_ = std::move(next);
        snapshotInitialized_ = true;
        if (selectedId_ > 0) {
            HubTask selected;
            if (!store_.task(selectedId_, selected)) selectedId_ = 0;
        }
        if (selectedId_ > 0) {
            detailEvents_ = store_.events(selectedId_);
            detailReply_ = store_.aiReply(selectedId_);
        } else {
            detailEvents_.clear();
            detailReply_.clear();
        }
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void refreshIntegrationStatus() {
        integrationStatus_ = integrations_.status();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
    }

    const HubTask *previousTask(std::int64_t id) const {
        for (const auto &task : snapshot_.queue) if (task.id == id) return &task;
        for (const auto &task : snapshot_.history) if (task.id == id) return &task;
        return nullptr;
    }

    void notifyTaskChanged(const HubTask &task) {
        if (!tray_.hWnd || !notificationsEnabled_) return;
        std::wstring eventLabel = L"任务状态更新";
        DWORD infoFlags = NIIF_INFO;
        if (task.eventType == L"TASK_COMPLETED") eventLabel = L"任务已完成";
        else if (task.eventType == L"TASK_FAILED") { eventLabel = L"任务执行失败"; infoFlags = NIIF_ERROR; }
        else if (task.eventType == L"TASK_NEEDS_INPUT") { eventLabel = L"任务等待输入"; infoFlags = NIIF_WARNING; }
        const std::wstring title = sourceLabel(task.source) + L" · " + eventLabel;
        std::wstring body = task.title.empty() ? L"未命名任务" : shorten(task.title, 100);
        if (!task.projectPath.empty()) body += L"\n" + shorten(task.projectPath, 120);
        NOTIFYICONDATAW notification = tray_;
        notification.uFlags = NIF_INFO;
        notification.dwInfoFlags = infoFlags;
        wcsncpy_s(notification.szInfoTitle, std::size(notification.szInfoTitle), title.c_str(), _TRUNCATE);
        wcsncpy_s(notification.szInfo, std::size(notification.szInfo), body.c_str(), _TRUNCATE);
        Shell_NotifyIconW(NIM_MODIFY, &notification);
    }

    // DEBUG: dump rendered BGRA buffer to PNG (one-shot, gated by env var or file flag).
    void dumpDebugPng() {
        wchar_t debugFlag[8]{};
        if (GetEnvironmentVariableW(L"AIHUB_DEBUG_RENDER", debugFlag,
                                    static_cast<DWORD>(std::size(debugFlag))) == 0)
            return;
        if (!backing_.bits || backing_.width <= 0 || backing_.height <= 0) return;
        static bool dumped = false;
        if (dumped) return;
        // 写到 %TEMP%\win32-debug-<pid>-<count>.bmp（最简 BMP 格式，无压缩）。
        wchar_t tempDir[MAX_PATH];
        GetTempPathW(MAX_PATH, tempDir);
        std::wstring path = std::wstring(tempDir) + L"win32-debug.bmp";
        FILE *fp = _wfopen(path.c_str(), L"wb");
        if (!fp) return;
        const int W = backing_.width, H = backing_.height;
        // BITMAPFILEHEADER
        BITMAPFILEHEADER bfh{};
        bfh.bfType = 0x4D42; // 'BM'
        bfh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
        bfh.bfSize = bfh.bfOffBits + W * H * 4;
        fwrite(&bfh, sizeof(bfh), 1, fp);
        // BITMAPINFOHEADER (positive height = bottom-up, matches our top-down DIB bit reversal)
        BITMAPINFOHEADER bih{};
        bih.biSize = sizeof(BITMAPINFOHEADER);
        bih.biWidth = W;
        bih.biHeight = H;
        bih.biPlanes = 1;
        bih.biBitCount = 32;
        bih.biCompression = BI_RGB;
        fwrite(&bih, sizeof(bih), 1, fp);
        // BGRA 数据：把 top-down DIB 翻成 bottom-up 写到 BMP
        std::vector<unsigned char> flipped(W * H * 4);
        for (int y = 0; y < H; ++y) {
            const unsigned char *src = static_cast<const unsigned char *>(backing_.bits) + (H - 1 - y) * W * 4;
            unsigned char *dst = flipped.data() + y * W * 4;
            // BMP 默认 BGRA；DIB 写入时也是 BGRA，所以无需交换通道。
            memcpy(dst, src, W * 4);
        }
        fwrite(flipped.data(), 1, W * H * 4, fp);
        fclose(fp);
        dumped = true;
    }

    void presentLayered() {
        if (!backing_.memDC || !hwnd_) return;
        HDC screenDC = GetDC(nullptr);
        RECT window{};
        GetWindowRect(hwnd_, &window);
        POINT ptSrc{0, 0};
        SIZE sizeWnd{backing_.width, backing_.height};
        POINT ptDst{window.left, window.top};
        BLENDFUNCTION blend{};
        blend.BlendOp = AC_SRC_OVER;
        blend.BlendFlags = 0;
        blend.SourceConstantAlpha = static_cast<BYTE>(std::clamp(opacityPercent_, 60, 100) * 255 / 100);
        blend.AlphaFormat = AC_SRC_ALPHA;
        UpdateLayeredWindow(hwnd_, screenDC, &ptDst, &sizeWnd, backing_.memDC, &ptSrc, 0, &blend, ULW_ALPHA);
        ReleaseDC(nullptr, screenDC);
    }

    void render() {
        if (!ensureFactory()) return;
        // 分层窗口提交的是整个窗口尺寸；若取客户区尺寸，WS_THICKFRAME 的边框会在每次
        // UpdateLayeredWindow 后被重复扣除，导致窗口每次重绘都变小。
        RECT window{};
        GetWindowRect(hwnd_, &window);
        const int width = window.right - window.left;
        const int height = window.bottom - window.top;
        if (!rebuildBacking(std::max(width, 1), std::max(height, 1))) return;
        if (!bindBacking()) return;
        // 首帧必须在 RenderTarget 建立后加载 WIC 图片，否则路径会被标记为已加载但 bitmap 仍为空。
        reloadImages();
        if (!brush_) {
            backing_.target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), brush_.GetAddressOf());
        }
        hits_.clear();
        if (backing_.target) {
            backing_.target->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_CLEARTYPE);
            backing_.target->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            backing_.target->BeginDraw();
            // 悬浮球模式只绘制圆形区域，先清空上一帧的面板内容并保留透明外围。
            backing_.target->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
            if (orbMode_) renderOrb(width, height);
            else renderPanel(width, height);
            const HRESULT result = backing_.target->EndDraw();
            if (result == D2DERR_RECREATE_TARGET) {
                brush_.Reset();
                destroyBacking();
            }
        }
        // DEBUG: dump rendered BGRA buffer to PNG once for diagnostic.
        presentLayered();
    }

    void renderPanel(int width, int height) {
        drawWallpaper(width, height);
        const Palette &p = palette();
        // 侧栏 180px：半透明 sidebar 底 + 右侧 1px 描边。
        fillRect(0, 46, 180, static_cast<float>(height), p.sidebar);
        line(180, 46, 180, static_cast<float>(height), 1.0f, p.border);
        renderTitlebar(width, height);
        renderSidebar(width, height);
        if (page_ == 2) renderSettings(width, height);
        else renderTasks(width, height);
        if (cleanupMenuOpen_ || clearConfirmationOpen_) renderCleanupOverlay(width, height);
    }

    void renderTitlebar(int width, int /*height*/) {
        const Palette &p = palette();
        // 标题栏渐变（top 0.55 → bottom 0）：两段 fill 实现 RGBA 渐变。
        // 为简化自绘，分 3 段拼接（顶/中/底）形成近似渐变。
        fillRect(0, 0, static_cast<float>(width), 12, p.titlebarTop);
        fillRect(0, 12, static_cast<float>(width), 30, blend(p.titlebarTop, 0.55f));
        fillRect(0, 30, static_cast<float>(width), 46, blend(p.titlebarTop, 0.18f));
        // 1px 底部分隔
        line(0, 46, static_cast<float>(width), 46, 1.0f, p.border);
        // 头像 + 标题副标题
        drawBitmapCircle(avatar_.Get(), 16, 10, 26);
        addHit(HitPickIcon, rectFrom(10, 5, 48, 41));
        if (hotHit_ == HitPickIcon) circleStroke(29, 23, 15, 1.4f, p.accentLine);
        text(L"AI Task Hub", 50, 12, 220, 32, 13.5f, p.textPrimary, true);
        text(L"多 AI 平台任务中心", 50, 30, 240, 44, 11.0f, p.muted);
        // 右侧状态与控制带起点 width - 280
        const int right = width;
        // 状态 pill 与右侧控制带保持 8px 间距。
        const int pillW = 68, pillH = 26, pillY = 10;
        const int pillX = right - 280;
        const D2D1_COLOR_F onlineDot = server_.running() ? p.success : p.danger;
        const bool hotPill = hotHit_ == HitQueue && false; // 状态 pill 暂不参与 hover
        fillRound(static_cast<float>(pillX), static_cast<float>(pillY),
                  static_cast<float>(pillX + pillW), static_cast<float>(pillY + pillH), 12.5f,
                  hotPill ? p.cardHover : p.tab);
        strokeRound(static_cast<float>(pillX), static_cast<float>(pillY),
                    static_cast<float>(pillX + pillW), static_cast<float>(pillY + pillH), 12.5f, 1, p.border);
        circle(static_cast<float>(pillX + 12), static_cast<float>(pillY + 13), 3, onlineDot);
        text(server_.running() ? L"已连接" : L"离线",
             static_cast<float>(pillX + 22), static_cast<float>(pillY + 5),
             static_cast<float>(pillX + pillW - 6), static_cast<float>(pillY + pillH - 4),
             10, p.textSecondary);
        // 右侧控制带统一为 36×36 单元格，保证图标中心、间距和点击区域一致。
        const int cell = 36, cellY = 5;
        const int themeX = right - 204;
        const int orbX = right - 164;
        // Windows 标准标题栏顺序：最小化 → 最大化/还原 → 关闭。
        const int minX = right - 124;
        const int zoomX = right - 84;
        const int closeX = right - 44;
        const auto drawControl = [&](int x, int id, bool hot, bool danger) {
            fillRound(static_cast<float>(x + 2), static_cast<float>(cellY + 2),
                      static_cast<float>(x + cell - 2), static_cast<float>(cellY + cell - 2), 16,
                      hot ? (danger ? p.dangerHover : p.cardHover) : D2D1::ColorF(0, 0, 0, 0));
            addHit(id, rectFrom(x, cellY, x + cell, cellY + cell));
        };
        const auto drawSun = [&](int x) {
            const float cx = static_cast<float>(x + cell / 2), cy = static_cast<float>(cellY + cell / 2);
            circleStroke(cx, cy, 4.5f, 1.2f, p.textSecondary);
            line(cx - 9, cy, cx - 6, cy, 1.2f, p.textSecondary);
            line(cx + 6, cy, cx + 9, cy, 1.2f, p.textSecondary);
            line(cx, cy - 9, cx, cy - 6, 1.2f, p.textSecondary);
            line(cx, cy + 6, cx, cy + 9, 1.2f, p.textSecondary);
        };
        const auto drawOrbIcon = [&](int x) {
            const float cx = static_cast<float>(x + cell / 2), cy = static_cast<float>(cellY + cell / 2);
            circleStroke(cx, cy, 7, 1.2f, p.textSecondary);
            circleStroke(cx, cy, 2.5f, 1.0f, p.textSecondary);
        };
        const auto drawMinimize = [&](int x) {
            const float cx = static_cast<float>(x + cell / 2), cy = static_cast<float>(cellY + cell / 2);
            line(cx - 7, cy, cx + 7, cy, 1.5f, p.textSecondary);
        };
        const auto drawZoom = [&](int x) {
            const float cx = static_cast<float>(x + cell / 2), cy = static_cast<float>(cellY + cell / 2);
            if (IsZoomed(hwnd_)) {
                strokeRound(cx - 1, cy - 1, cx + 8, cy + 8, 1.5f, 1.2f, p.textSecondary);
                strokeRound(cx - 8, cy - 8, cx + 1, cy + 1, 1.5f, 1.2f, p.textSecondary);
            } else {
                strokeRound(cx - 7, cy - 7, cx + 7, cy + 7, 2, 1.3f, p.textSecondary);
            }
        };
        const auto drawClose = [&](int x) {
            const float cx = static_cast<float>(x + cell / 2), cy = static_cast<float>(cellY + cell / 2);
            const D2D1_COLOR_F icon = hotHit_ == HitClose ? color(0xffffff) : p.textSecondary;
            line(cx - 6, cy - 6, cx + 6, cy + 6, 1.4f, icon);
            line(cx + 6, cy - 6, cx - 6, cy + 6, 1.4f, icon);
        };
        drawControl(themeX, HitThemeToggle, hotHit_ == HitThemeToggle, false);
        drawSun(themeX);
        drawControl(orbX, HitEnterOrb, hotHit_ == HitEnterOrb, false);
        drawOrbIcon(orbX);
        drawControl(minX, HitMinimize, hotHit_ == HitMinimize, false);
        drawMinimize(minX);
        drawControl(zoomX, HitMaximize, hotHit_ == HitMaximize, false);
        drawZoom(zoomX);
        drawControl(closeX, HitClose, hotHit_ == HitClose, true);
        drawClose(closeX);
    }

    void renderSidebar(int /*width*/, int height) {
        const Palette &p = palette();
        // "任务" 小节标题
        text(L"任务", 22, 64, 168, 84, 10.5f, p.muted, true);
        // 3 个 nav item
        renderNavItem(90, HitQueue, L"待处理", L"\u25FB", page_ == 0, snapshot_.counts.queue, true);
        renderNavItem(135, HitHistory, L"历史", L"\u25F7", page_ == 1, snapshot_.counts.history, true);
        renderNavItem(180, HitSettings, L"设置", L"\u2699", page_ == 2, 0, false);
        // 底部 footer
        const int footerTop = height - 110;
        line(14, footerTop, 166, footerTop, 1.0f, p.border);
        const D2D1_COLOR_F serverDot = server_.running() ? p.success : p.danger;
        circle(20, static_cast<float>(footerTop + 16), 3, serverDot);
        text(server_.running() ? L"本地服务在线" : L"本地服务离线",
             30, static_cast<float>(footerTop + 9), 166, static_cast<float>(footerTop + 26),
             12, p.textPrimary);
        text(server_.running() ? L"127.0.0.1:17891" : serverWarning_,
             14, static_cast<float>(footerTop + 32), 166, static_cast<float>(footerTop + 50),
             11, p.textSecondary);
        text(server_.running() ? L"事件驱动 · 本地运行" : L"正在初始化 SQLite",
             14, static_cast<float>(footerTop + 53), 166, static_cast<float>(footerTop + 70),
             10, p.muted);
        text(L"Claude Code · Codex ·\nChatGPT",
             14, static_cast<float>(footerTop + 76), 166, static_cast<float>(footerTop + 110),
             11, p.muted, false, true);
    }

    void renderNavItem(int top, int id, const wchar_t *label, const wchar_t *icon,
                       bool selected, int count, bool showBadge) {
        const Palette &p = palette();
        const int left = 14, right = 166, h = 40;
        if (selected) {
            fillRound(static_cast<float>(left), static_cast<float>(top),
                      static_cast<float>(right), static_cast<float>(top + h), 20, p.accentSoft);
            strokeRound(static_cast<float>(left), static_cast<float>(top),
                        static_cast<float>(right), static_cast<float>(top + h), 20, 1, p.accentLine);
            // 左侧 3px 高亮条
            fillRound(static_cast<float>(left), static_cast<float>(top + 6),
                      static_cast<float>(left + 3), static_cast<float>(top + h - 6), 2, p.accent);
        }
        addHit(id, rectFrom(left, top, right, top + h));
        // icon
        text(icon, static_cast<float>(left + 12), static_cast<float>(top + 10),
             static_cast<float>(left + 28), static_cast<float>(top + 30),
             15, selected ? p.textPrimary : p.textSecondary);
        // label
        text(label, static_cast<float>(left + 36), static_cast<float>(top + 12),
             static_cast<float>(right - 36), static_cast<float>(top + 32),
             12.5f, p.textPrimary);
        if (showBadge && count > 0) {
            const std::wstring number = count > 99 ? L"99+" : std::to_wstring(count);
            const int badgeW = 19 + (number.size() > 1 ? 6 : 0);
            const int badgeX = right - badgeW - 8;
            fillRound(static_cast<float>(badgeX), static_cast<float>(top + 11),
                      static_cast<float>(badgeX + badgeW), static_cast<float>(top + 29), 9.5f, p.accent);
            textCentered(number, static_cast<float>(badgeX), static_cast<float>(top + 11),
                         static_cast<float>(badgeX + badgeW), static_cast<float>(top + 29),
                         11, color(0xffffff), true);
        }
    }

    void renderTasks(int width, int height) {
        const bool history = page_ == 1;
        history_ = history;
        const int left = 200;
        const int right = width - 22;
        const Palette &p = palette();
        // 顶部标题 22px Segoe UI Variable Display（用 textFormat size 模拟）
        text(history ? L"历史" : L"待处理",
             static_cast<float>(left), 78, static_cast<float>(right - 350), 116,
             22, p.textPrimary, true);
        const std::wstring summary = history
            ? (std::to_wstring(snapshot_.counts.history) + L" 条历史任务")
            : (std::to_wstring(snapshot_.counts.queue) + L" 个任务需要你的注意");
        text(summary, static_cast<float>(left), 116, static_cast<float>(right - 350), 138,
             12, p.textSecondary);
        // 右上角操作按钮：清理 + 一键已读 + 收起为悬浮球
        const int btnY = 78, btnH = 34;
        const int clearX = right - 330;
        const bool hotClear = hotHit_ == HitCleanupMenu;
        fillRound(static_cast<float>(clearX), static_cast<float>(btnY),
                  static_cast<float>(clearX + 100), static_cast<float>(btnY + btnH), 17,
                  hotClear ? p.cardHover : p.tab);
        strokeRound(static_cast<float>(clearX), static_cast<float>(btnY),
                    static_cast<float>(clearX + 100), static_cast<float>(btnY + btnH), 17, 1, p.border);
        text(L"清理", static_cast<float>(clearX + 12), static_cast<float>(btnY + 9),
             static_cast<float>(clearX + 100 - 12), static_cast<float>(btnY + btnH - 8),
             12, p.textSecondary);
        addHit(HitCleanupMenu, rectFrom(clearX, btnY, clearX + 100, btnY + btnH));
        const int markX = right - 220;
        const bool hotMark = hotHit_ == HitMarkAll;
        fillRound(static_cast<float>(markX), static_cast<float>(btnY),
                  static_cast<float>(markX + 100), static_cast<float>(btnY + btnH), 17,
                  hotMark ? p.cardHover : p.tab);
        strokeRound(static_cast<float>(markX), static_cast<float>(btnY),
                    static_cast<float>(markX + 100), static_cast<float>(btnY + btnH), 17, 1, p.border);
        text(L"一键已读", static_cast<float>(markX + 12), static_cast<float>(btnY + 9),
             static_cast<float>(markX + 100 - 12), static_cast<float>(btnY + btnH - 8),
             12, p.textSecondary);
        addHit(HitMarkAll, rectFrom(markX, btnY, markX + 100, btnY + btnH));
        const int orbX = right - 110;
        const bool hotOrb = hotHit_ == HitOrb;
        fillRound(static_cast<float>(orbX), static_cast<float>(btnY),
                  static_cast<float>(orbX + 100), static_cast<float>(btnY + btnH), 17,
                  hotOrb ? p.cardHover : p.tab);
        strokeRound(static_cast<float>(orbX), static_cast<float>(btnY),
                    static_cast<float>(orbX + 100), static_cast<float>(btnY + btnH), 17, 1, p.border);
        text(L"收起为悬浮球", static_cast<float>(orbX + 10), static_cast<float>(btnY + 9),
             static_cast<float>(orbX + 100 - 10), static_cast<float>(btnY + btnH - 8),
             12, p.textSecondary);
        addHit(HitOrb, rectFrom(orbX, btnY, orbX + 100, btnY + btnH));

        // 筛选条
        renderFilterRow(left, 154, right - left, history);
        // 搜索框底部在 234..274，列表从 290 开始并独立裁剪，滚动时不会盖住筛选区。
        const int contentTop = 290;
        const bool hasDetail = selectedId_ > 0;
        if (hasDetail && width < 1180) {
            renderDetail(width, height, left, contentTop, right);
            return;
        }
        const int detailWidth = hasDetail ? 360 : 0;
        const int listRight = right - detailWidth - (hasDetail ? 18 : 0);
        const int columns = listRight - left >= 630 ? 2 : 1;
        const int gap = 14;
        const int cardWidth = columns == 2 ? (listRight - left - gap) / 2 : listRight - left;
        const auto &all = history ? snapshot_.history : snapshot_.queue;
        std::vector<HubTask> visible;
        for (const auto &task : all) {
            if (!sourceFilter_.empty() && task.source != sourceFilter_) continue;
            if (!statusFilter_.empty() && statusFilter_ != L"ALL" && task.status != statusFilter_) continue;
            if (!search_.empty()) {
                const auto contains = [this](const std::wstring &value) {
                    return FindStringOrdinal(FIND_FROMSTART, value.c_str(), static_cast<int>(value.size()),
                                             search_.c_str(), static_cast<int>(search_.size()), TRUE) >= 0;
                };
                if (!contains(task.title) && !contains(task.contentPreview) && !contains(task.projectPath)) continue;
            }
            visible.push_back(task);
        }
        const int cardHeight = 146;
        const int rowHeight = cardHeight + gap;
        const int rows = static_cast<int>((visible.size() + columns - 1) / columns);
        const int maxScroll = std::max(0, rows * rowHeight - (height - contentTop - 16));
        scrollOffset_ = std::clamp(scrollOffset_, 0, maxScroll);
        target()->PushAxisAlignedClip(D2D1::RectF(0, static_cast<float>(contentTop),
                                                   static_cast<float>(width), static_cast<float>(height - 14)),
                                      D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        setHitClip(rectFrom(left, contentTop, listRight, height - 14));
        for (size_t index = 0; index < visible.size(); ++index) {
            const int row = static_cast<int>(index) / columns;
            const int col = static_cast<int>(index) % columns;
            const int x = left + col * (cardWidth + gap);
            const int y = contentTop + row * rowHeight - scrollOffset_;
            if (y + cardHeight < contentTop || y > height - 16) continue;
            renderTaskCard(visible[index], x, y, cardWidth, cardHeight);
        }
        if (visible.empty()) {
            const bool filtered = !search_.empty() || !sourceFilter_.empty() || !statusFilter_.empty();
            const int emptyTop = contentTop + 18;
            fillRound(static_cast<float>(left), static_cast<float>(emptyTop),
                      static_cast<float>(listRight), static_cast<float>(emptyTop + 156), 18, taskCardSurface());
            circle(static_cast<float>((left + listRight) / 2), static_cast<float>(emptyTop + 60), 28, p.accentSoft);
            textCentered(filtered ? L"没有匹配的任务" : (history ? L"暂无历史任务" : L"全部处理完毕"),
                 static_cast<float>(left + 20), static_cast<float>(emptyTop + 96),
                 static_cast<float>(listRight - 20), static_cast<float>(emptyTop + 122),
                 15, p.textPrimary, true);
            textCentered(filtered ? L"清除筛选，查看全部任务" : (history ? L"已查看和已忽略的任务会保留在这里" : L"新的 AI 任务完成时会实时推送到这里"),
                 static_cast<float>(left + 20), static_cast<float>(emptyTop + 122),
                 static_cast<float>(listRight - 20), static_cast<float>(emptyTop + 148),
                 12, filtered ? p.accent : p.muted);
            if (filtered) addHit(HitResetFilters, rectFrom(left + 20, emptyTop + 122, listRight - 20, emptyTop + 148));
        }
        clearHitClip();
        target()->PopAxisAlignedClip();
        if (hasDetail) renderDetail(width, height, listRight + 18, contentTop, right);
    }

    void renderFilterRow(int left, int top, int width, bool history) {
        const Palette &p = palette();
        // 状态 chips：30px 高，5 个
        struct StatusOption { const wchar_t *code; const wchar_t *label; D2D1_COLOR_F color; };
        const std::vector<StatusOption> queueOpts{
            {L"ALL", L"全部", p.muted},
            {L"RUNNING", L"执行中", color(0x38bdf8)},
            {L"NEEDS_INPUT", L"等待输入", color(0xf59e0b)},
            {L"COMPLETED_UNREAD", L"已完成", color(0x22c55e)},
            {L"FAILED_UNREAD", L"失败", color(0xef4444)},
        };
        const std::vector<StatusOption> histOpts{
            {L"ALL", L"全部", p.muted},
            {L"VIEWED", L"已查看", color(0x22c55e)},
            {L"IGNORED", L"已忽略", p.muted},
        };
        const auto &opts = history ? histOpts : queueOpts;
        int x = left;
        for (size_t i = 0; i < opts.size(); ++i) {
            const std::wstring label = std::wstring(opts[i].label);
            const int chipW = (width - 7 * (static_cast<int>(opts.size()) - 1)) / static_cast<int>(opts.size());
            const bool selected = (i == 0 && statusFilter_.empty()) || statusFilter_ == opts[i].code;
            fillRound(static_cast<float>(x), static_cast<float>(top),
                      static_cast<float>(x + chipW), static_cast<float>(top + 30), 15,
                      selected ? p.tabActive : D2D1::ColorF(0, 0, 0, 0));
            strokeRound(static_cast<float>(x), static_cast<float>(top),
                        static_cast<float>(x + chipW), static_cast<float>(top + 30), 15, 1,
                        selected ? p.borderStrong : D2D1::ColorF(0, 0, 0, 0));
            circle(static_cast<float>(x + 12), static_cast<float>(top + 15), 3, opts[i].color);
            text(label, static_cast<float>(x + 22), static_cast<float>(top + 7),
                 static_cast<float>(x + chipW - 30), static_cast<float>(top + 24),
                 12, selected ? p.textPrimary : p.muted, selected);
            // count
            int cnt = 0;
            if (history) {
                if (i == 0) cnt = snapshot_.counts.history;
                else if (opts[i].code == L"VIEWED") cnt = snapshot_.counts.viewed;
                else if (opts[i].code == L"IGNORED") cnt = snapshot_.counts.ignored;
            } else {
                if (i == 0) cnt = snapshot_.counts.queue;
                else if (opts[i].code == L"RUNNING") cnt = snapshot_.counts.running;
                else if (opts[i].code == L"NEEDS_INPUT") cnt = snapshot_.counts.needsInput;
                else if (opts[i].code == L"COMPLETED_UNREAD") cnt = snapshot_.counts.completedUnread;
                else if (opts[i].code == L"FAILED_UNREAD") cnt = snapshot_.counts.failedUnread;
            }
            text(std::to_wstring(cnt), static_cast<float>(x + chipW - 28), static_cast<float>(top + 8),
                 static_cast<float>(x + chipW - 8), static_cast<float>(top + 24),
                 11, p.muted);
            addHit(HitStatusBase + static_cast<int>(i), rectFrom(x, top, x + chipW, top + 30));
            x += chipW + 7;
        }
        // 来源 chips
        struct SourceOption { const wchar_t *id; const wchar_t *label; D2D1_COLOR_F color; };
        const std::vector<SourceOption> sources{
            {L"", L"全部来源", p.muted},
            {L"CHATGPT", L"GPT 网页", color(0x10a37f)},
            {L"CLAUDE_CODE", L"Claude Code", color(0xd97757)},
            {L"CODEX", L"Codex", color(0x4f8ff7)},
            {L"OTHER", L"其他", color(0x8b5cf6)},
        };
        const int srcY = top + 40;
        x = left;
        for (size_t i = 0; i < sources.size(); ++i) {
            const std::wstring label = std::wstring(sources[i].label);
            const int chipW = (width - 7 * 4) / 5;
            const bool selected = (i == 0 && sourceFilter_.empty()) || sourceFilter_ == sources[i].id;
            fillRound(static_cast<float>(x), static_cast<float>(srcY),
                      static_cast<float>(x + chipW), static_cast<float>(srcY + 30), 15,
                      selected ? p.tabActive : D2D1::ColorF(0, 0, 0, 0));
            strokeRound(static_cast<float>(x), static_cast<float>(srcY),
                        static_cast<float>(x + chipW), static_cast<float>(srcY + 30), 15, 1,
                        selected ? p.borderStrong : D2D1::ColorF(0, 0, 0, 0));
            if (i > 0) circle(static_cast<float>(x + 12), static_cast<float>(srcY + 15), 3, sources[i].color);
            text(label, static_cast<float>(x + (i > 0 ? 22 : 12)), static_cast<float>(srcY + 7),
                 static_cast<float>(x + chipW - 30), static_cast<float>(srcY + 24),
                 12, selected ? p.textPrimary : p.muted, selected);
            // count
            int cnt = 0;
            const auto &list = history ? snapshot_.history : snapshot_.queue;
            for (const auto &t : list) {
                if (i == 0 || t.source == sources[i].id) ++cnt;
            }
            text(std::to_wstring(cnt), static_cast<float>(x + chipW - 28), static_cast<float>(srcY + 8),
                 static_cast<float>(x + chipW - 8), static_cast<float>(srcY + 24),
                 11, p.muted);
            addHit(HitSourceBase + static_cast<int>(i), rectFrom(x, srcY, x + chipW, srcY + 30));
            x += chipW + 7;
        }
        // 搜索框 40px 高圆角 999
        const int sX = left, sY = top + 80, sW = width, sH = 40;
        fillRound(static_cast<float>(sX), static_cast<float>(sY),
                  static_cast<float>(sX + sW), static_cast<float>(sY + sH), 20, p.tab);
        strokeRound(static_cast<float>(sX), static_cast<float>(sY),
                    static_cast<float>(sX + sW), static_cast<float>(sY + sH), 20, 1, p.border);
        text(L"\u2315", static_cast<float>(sX + 16), static_cast<float>(sY + 9),
             static_cast<float>(sX + 40), static_cast<float>(sY + 32),
             19, p.muted);
        const std::wstring ph = search_.empty() ? std::wstring(L"搜索标题 / 路径 / 摘要…") : search_;
        const D2D1_COLOR_F txtColor = search_.empty() ? p.muted : p.textPrimary;
        text(ph, static_cast<float>(sX + 46), static_cast<float>(sY + 12),
             static_cast<float>(sX + sW - 90), static_cast<float>(sY + sH - 10),
             12, txtColor);
        // Ctrl K 提示
        const int kX = sX + sW - 70, kY = sY + 10, kW = 56, kH = 20;
        fillRound(static_cast<float>(kX), static_cast<float>(kY),
                  static_cast<float>(kX + kW), static_cast<float>(kY + kH), 6, p.cardHover);
        strokeRound(static_cast<float>(kX), static_cast<float>(kY),
                    static_cast<float>(kX + kW), static_cast<float>(kY + kH), 6, 1, p.border);
        textCentered(L"Ctrl K", static_cast<float>(kX), static_cast<float>(kY),
             static_cast<float>(kX + kW), static_cast<float>(kY + kH),
             10, p.muted);
        addHit(HitSearch, rectFrom(sX, sY, sX + sW, sY + sH));
    }

    void renderTaskCard(const HubTask &task, int x, int y, int width, int height) {
        const bool selected = task.id == selectedId_;
        const int cardBaseId = HitCardBase + static_cast<int>(task.id);
        const bool hot = hotTaskId_ == task.id;
        addHit(cardBaseId, rectFrom(x, y, x + width, y + height), task.id);
        const Palette &p = palette();
        // 卡片底
        fillRound(static_cast<float>(x), static_cast<float>(y),
                  static_cast<float>(x + width), static_cast<float>(y + height), 18,
                  taskCardSurface(selected || hot));
        strokeRound(static_cast<float>(x), static_cast<float>(y),
                    static_cast<float>(x + width), static_cast<float>(y + height), 18, 1,
                    selected ? p.accentLine : p.border);
        // 状态色条 4×(h-32) 居中
        const int barX = x + 8, barY = y + 16, barH = height - 32;
        fillRound(static_cast<float>(barX), static_cast<float>(barY),
                  static_cast<float>(barX + 4), static_cast<float>(y + 16 + barH), 2,
                  statusColor(task.status));
        // 顶部行：sourcePill + · + statusLabel + flex + formatTime
        const D2D1_COLOR_F source = sourceColor(task.source);
        const std::wstring sName = sourceLabel(task.source);
        const int pillW = 28 + static_cast<int>(sName.size() * 8);
        const int pillX = x + 21, pillY = y + 14, pillH = 22;
        fillRound(static_cast<float>(pillX), static_cast<float>(pillY),
                  static_cast<float>(pillX + pillW), static_cast<float>(pillY + pillH), 11,
                  blend(source, 0.14f));
        strokeRound(static_cast<float>(pillX), static_cast<float>(pillY),
                    static_cast<float>(pillX + pillW), static_cast<float>(pillY + pillH), 11, 1,
                    blend(source, 0.30f));
        circle(static_cast<float>(pillX + 8), static_cast<float>(pillY + 11), 2.5f, source);
        text(sName, static_cast<float>(pillX + 16), static_cast<float>(pillY + 4),
             static_cast<float>(pillX + pillW - 4), static_cast<float>(pillY + pillH - 2),
             11, source, true);
        const int dotX = pillX + pillW + 6;
        text(L"·", static_cast<float>(dotX), static_cast<float>(pillY + 2),
             static_cast<float>(dotX + 8), static_cast<float>(pillY + pillH - 2),
             12, p.muted);
        text(statusLabel(task.status),
             static_cast<float>(dotX + 8), static_cast<float>(pillY + 4),
             static_cast<float>(x + width - 92), static_cast<float>(pillY + pillH - 2),
             11, statusColor(task.status));
        const std::wstring ts = formatTime(task.completedAt.empty() ? task.createdAt : task.completedAt);
        text(ts, static_cast<float>(x + width - 86), static_cast<float>(pillY + 5),
             static_cast<float>(x + width - 12), static_cast<float>(pillY + pillH - 2),
             10, p.muted);
        // 标题 13.5px SemiBold
        text(shorten(task.title.empty() ? L"未命名任务" : task.title, 56),
             static_cast<float>(x + 21), static_cast<float>(y + 50),
             static_cast<float>(x + width - 14), static_cast<float>(y + 92),
             13.5f, p.textPrimary, true, true);
        // 预览 12px secondary 1 行
        text(shorten(task.contentPreview.empty() ? L"暂无摘要" : task.contentPreview, 84),
             static_cast<float>(x + 21), static_cast<float>(y + 95),
             static_cast<float>(x + width - 14), static_cast<float>(y + 116),
             12, p.textSecondary);
        // 底部 footer：项目路径 + 忽略/打开
        const int footY = y + height - 26;
        text(shorten(task.projectPath, 40), static_cast<float>(x + 21), static_cast<float>(footY),
             static_cast<float>(x + width - 130), static_cast<float>(footY + 16),
             10.5f, p.muted);
        if (hot) {
            // mini 忽略
            const int igX = x + width - 124, igY = footY - 4, igW = 50, igH = 22;
            fillRound(static_cast<float>(igX), static_cast<float>(igY),
                      static_cast<float>(igX + igW), static_cast<float>(igY + igH), 11,
                      D2D1::ColorF(0, 0, 0, 0));
            strokeRound(static_cast<float>(igX), static_cast<float>(igY),
                        static_cast<float>(igX + igW), static_cast<float>(igY + igH), 11, 1, p.border);
            textCentered(history_ ? L"删除" : L"忽略",
                 static_cast<float>(igX), static_cast<float>(igY),
                 static_cast<float>(igX + igW), static_cast<float>(igY + igH),
                 11, p.textSecondary);
            addHit(HitCardIgnore, rectFrom(igX, igY, igX + igW, igY + igH), task.id);
            // primary 打开
            const int opX = x + width - 66, opY = footY - 4, opW = 54, opH = 22;
            fillRound(static_cast<float>(opX), static_cast<float>(opY),
                      static_cast<float>(opX + opW), static_cast<float>(opY + opH), 11,
                      p.accentSoft);
            strokeRound(static_cast<float>(opX), static_cast<float>(opY),
                        static_cast<float>(opX + opW), static_cast<float>(opY + opH), 11, 1,
                        p.accentLine);
            textCentered(L"打开", static_cast<float>(opX), static_cast<float>(opY),
                 static_cast<float>(opX + opW), static_cast<float>(opY + opH),
                 11, p.accent, true);
            addHit(HitCardOpen, rectFrom(opX, opY, opX + opW, opY + opH), task.id);
        }
    }

    void renderDetail(int width, int height, int left, int top, int right) {
        HubTask task;
        if (!store_.task(selectedId_, task)) return;
        const Palette &p = palette();
        const int detailBottom = height - 22;
        // 卡片底
        fillRound(static_cast<float>(left), static_cast<float>(top),
                  static_cast<float>(right), static_cast<float>(detailBottom), 18, taskCardSurface());
        strokeRound(static_cast<float>(left), static_cast<float>(top),
                    static_cast<float>(right), static_cast<float>(detailBottom), 18, 1, p.border);
        // 头部
        text(L"任务详情", static_cast<float>(left + 20), static_cast<float>(top + 16),
             static_cast<float>(right - 50), static_cast<float>(top + 36),
             14, p.textPrimary, true);
        const int closeX = right - 46, closeY = top + 12, closeW = 30, closeH = 30;
        const bool hotX = hotHit_ == HitCloseDetail;
        fillRound(static_cast<float>(closeX), static_cast<float>(closeY),
                  static_cast<float>(closeX + closeW), static_cast<float>(closeY + closeH), 15,
                  hotX ? p.cardHover : D2D1::ColorF(0, 0, 0, 0));
        text(L"\u00D7", static_cast<float>(closeX + 8), static_cast<float>(closeY + 6),
             static_cast<float>(closeX + closeW - 6), static_cast<float>(closeY + closeH - 4),
             18, p.textSecondary);
        addHit(HitCloseDetail, rectFrom(closeX, closeY, closeX + closeW, closeY + closeH));
        // 分隔线
        line(static_cast<float>(left + 20), static_cast<float>(top + 50),
             static_cast<float>(right - 20), static_cast<float>(top + 50), 1, p.border);
        const int btnY = detailBottom - 42;
        const int viewportTop = top + 58, viewportBottom = btnY - 12;
        const int titleHeight = std::max(24, wrappedHeight(task.title, right - left - 40, 15, true));
        const int previewHeight = wrappedHeight(task.contentPreview, right - left - 40, 12);
        const int replyHeight = wrappedHeight(detailReply_, right - left - 56, 12);
        const int eventHeight = std::max(1, static_cast<int>(detailEvents_.size())) * 22;
        const int contentHeight = titleHeight + previewHeight + eventHeight + 106 + (detailReply_.empty() ? 0 : replyHeight + 48);
        detailScrollOffset_ = std::clamp(detailScrollOffset_, 0, std::max(0, contentHeight - (viewportBottom - viewportTop)));
        detailViewport_ = rectFrom(left, viewportTop, right, viewportBottom);
        target()->PushAxisAlignedClip(D2D1::RectF(left, viewportTop, right, viewportBottom), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        int contentY = viewportTop - detailScrollOffset_;
        text(task.title, static_cast<float>(left + 20), static_cast<float>(contentY),
             static_cast<float>(right - 20), static_cast<float>(contentY + titleHeight),
             15, p.textPrimary, true, true);
        contentY += titleHeight + 12;
        // 来源（accent）+ 状态（status色）+ flex + 时间
        text(sourceLabel(task.source), static_cast<float>(left + 20), static_cast<float>(contentY),
             static_cast<float>(left + 100), static_cast<float>(contentY + 20),
             11, p.accent, true);
        text(statusLabel(task.status), static_cast<float>(left + 104), static_cast<float>(contentY),
             static_cast<float>(right - 110), static_cast<float>(contentY + 20),
             11, statusColor(task.status));
        text(formatTime(task.createdAt), static_cast<float>(right - 110), static_cast<float>(contentY),
             static_cast<float>(right - 20), static_cast<float>(contentY + 20),
             10, p.muted);
        contentY += 30;
        text(task.contentPreview, static_cast<float>(left + 20), static_cast<float>(contentY),
             static_cast<float>(right - 20), static_cast<float>(contentY + previewHeight),
             12, p.textSecondary, false, true);
        // 生命周期
        contentY += previewHeight + 16;
        text(L"生命周期", static_cast<float>(left + 20), static_cast<float>(contentY),
             static_cast<float>(right - 20), static_cast<float>(contentY + 20),
             12, p.muted, true);
        int ey = contentY + 26;
        for (const auto &ev : detailEvents_) {
            circle(static_cast<float>(left + 26), static_cast<float>(ey + 6), 3.5f, p.accent);
            const std::wstring label = statusLabel(ev.eventType);
            text(label, static_cast<float>(left + 38), static_cast<float>(ey),
                 static_cast<float>(right - 90), static_cast<float>(ey + 14),
                 11, p.textPrimary);
            text(formatTime(ev.createdAt), static_cast<float>(right - 90), static_cast<float>(ey + 1),
                 static_cast<float>(right - 20), static_cast<float>(ey + 14),
                 10, p.muted);
            ey += 22;
        }
        if (detailEvents_.empty()) {
            text(L"暂无事件", static_cast<float>(left + 20), static_cast<float>(ey),
                 static_cast<float>(right - 20), static_cast<float>(ey + 14),
                 11, p.muted);
        }
        // AI 答复
        if (!detailReply_.empty()) {
            int replyTop = contentY + 26 + eventHeight + 16;
            text(L"AI 答复", static_cast<float>(left + 20), static_cast<float>(replyTop),
                 static_cast<float>(right - 20), static_cast<float>(replyTop + 16),
                 12, p.muted, true);
            const int boxTop = replyTop + 22;
            const int boxH = replyHeight + 16;
            fillRound(static_cast<float>(left + 20), static_cast<float>(boxTop),
                      static_cast<float>(right - 20), static_cast<float>(boxTop + boxH), 10, p.canvas);
            strokeRound(static_cast<float>(left + 20), static_cast<float>(boxTop),
                        static_cast<float>(right - 20), static_cast<float>(boxTop + boxH), 10, 1, p.border);
            text(detailReply_, static_cast<float>(left + 28), static_cast<float>(boxTop + 6),
                 static_cast<float>(right - 28), static_cast<float>(boxTop + boxH - 6),
                 12, p.textPrimary, false, true);
        }
        target()->PopAxisAlignedClip();
        // 按钮行：打开（primary flex）+ 已读（ghost if != VIEWED）+ 删除（mini）
        const int openX = left + 20, openW = (right - left) / 2 - 28;
        const bool hotOpen = hotHit_ == HitDetailOpen;
        fillRound(static_cast<float>(openX), static_cast<float>(btnY),
                  static_cast<float>(openX + openW), static_cast<float>(btnY + 32), 14,
                  p.accentSoft);
        strokeRound(static_cast<float>(openX), static_cast<float>(btnY),
                    static_cast<float>(openX + openW), static_cast<float>(btnY + 32), 14, 1, p.accentLine);
        textCentered(L"打开", static_cast<float>(openX), static_cast<float>(btnY),
             static_cast<float>(openX + openW), static_cast<float>(btnY + 32),
             12, p.accent, true);
        addHit(HitDetailOpen, rectFrom(openX, btnY, openX + openW, btnY + 32));
        const int readX = openX + openW + 8, readW = 60;
        if (task.status != L"VIEWED") {
            const bool hotRead = hotHit_ == HitDetailRead;
            fillRound(static_cast<float>(readX), static_cast<float>(btnY),
                      static_cast<float>(readX + readW), static_cast<float>(btnY + 32), 14,
                      hotRead ? p.cardHover : p.tab);
            strokeRound(static_cast<float>(readX), static_cast<float>(btnY),
                        static_cast<float>(readX + readW), static_cast<float>(btnY + 32), 14, 1, p.border);
            textCentered(L"已读", static_cast<float>(readX), static_cast<float>(btnY),
                 static_cast<float>(readX + readW), static_cast<float>(btnY + 32),
                 12, p.textSecondary);
            addHit(HitDetailRead, rectFrom(readX, btnY, readX + readW, btnY + 32));
        }
        const int delX = right - 78;
        const bool hotDel = hotHit_ == HitDetailDelete;
        fillRound(static_cast<float>(delX), static_cast<float>(btnY),
                  static_cast<float>(delX + 60), static_cast<float>(btnY + 32), 14,
                  hotDel ? p.cardHover : D2D1::ColorF(0, 0, 0, 0));
        strokeRound(static_cast<float>(delX), static_cast<float>(btnY),
                    static_cast<float>(delX + 60), static_cast<float>(btnY + 32), 14, 1, p.border);
        textCentered(L"删除", static_cast<float>(delX), static_cast<float>(btnY),
             static_cast<float>(delX + 60), static_cast<float>(btnY + 32),
             12, p.textSecondary);
        addHit(HitDetailDelete, rectFrom(delX, btnY, delX + 60, btnY + 32));
        (void)width;
    }

    void renderIntegrationAction(int left, int top, int width, const wchar_t *title,
                                 const wchar_t *pathLabel, const wchar_t *state,
                                 const wchar_t *buttonLabel, int hitId, bool online) {
        const Palette &p = palette();
        const int height = 88;
        const bool hot = hotHit_ == hitId;
        fillRound(static_cast<float>(left), static_cast<float>(top),
                  static_cast<float>(left + width), static_cast<float>(top + height), 12,
                  darkMode_ ? (hot ? p.cardHover : p.card) : colorFromArgb(hot ? 0x70ffffffu : 0x38ffffffu));
        strokeRound(static_cast<float>(left), static_cast<float>(top),
                    static_cast<float>(left + width), static_cast<float>(top + height), 12, 1,
                    online ? blend(p.success, 0.55f) : p.border);
        text(title, static_cast<float>(left + 12), static_cast<float>(top + 10),
             static_cast<float>(left + width - 124), static_cast<float>(top + 29),
             13, p.textPrimary, true);
        text(pathLabel, static_cast<float>(left + 12), static_cast<float>(top + 31),
             static_cast<float>(left + width - 124), static_cast<float>(top + 52),
             11, p.textSecondary);
        circle(static_cast<float>(left + 14), static_cast<float>(top + 62), 3,
               online ? p.success : p.muted);
        text(state, static_cast<float>(left + 23), static_cast<float>(top + 53),
             static_cast<float>(left + width - 104), static_cast<float>(top + 72),
             10, online ? p.success : p.textSecondary);
        const int buttonWidth = std::wstring(buttonLabel).size() > 5 ? 94 : 78;
        const int buttonLeft = left + width - buttonWidth - 10;
        const int buttonTop = top + 23;
        fillRound(static_cast<float>(buttonLeft), static_cast<float>(buttonTop),
                  static_cast<float>(buttonLeft + buttonWidth), static_cast<float>(buttonTop + 30), 15,
                  hot ? p.accentSoft : p.tab);
        strokeRound(static_cast<float>(buttonLeft), static_cast<float>(buttonTop),
                    static_cast<float>(buttonLeft + buttonWidth), static_cast<float>(buttonTop + 30), 15, 1,
                    online ? p.success : p.border);
        textCentered(buttonLabel, static_cast<float>(buttonLeft), static_cast<float>(buttonTop),
                     static_cast<float>(buttonLeft + buttonWidth), static_cast<float>(buttonTop + 30),
                     10.5f, online ? p.success : p.textSecondary, true);
        addHit(hitId, rectFrom(buttonLeft, buttonTop, buttonLeft + buttonWidth, buttonTop + 30));
    }

    void renderSettings(int width, int height) {
        const Palette &p = palette();
        const int left = 200, right = width - 22;
        // 顶部大标题
        text(L"设置", static_cast<float>(left), 78, static_cast<float>(right), 116,
             22, p.textPrimary, true);
        // 取消页头说明后，把卡片上移到标题下方，避免留下大块空白。
        const wchar_t *tabs[] = {L"外观主题", L"平台接入", L"数据与通知"};
        for (int i = 0; i < 3; ++i) {
            const int tabLeft = left + i * 132;
            const bool active = settingsTab_ == i;
            fillRound(tabLeft, 118, tabLeft + 124, 150, 12, active ? p.accentSoft : p.tab);
            strokeRound(tabLeft, 118, tabLeft + 124, 150, 12, 1, active ? p.accentLine : p.border);
            textCentered(tabs[i], tabLeft, 118, tabLeft + 124, 150, 12, active ? p.accent : p.textSecondary, active);
            addHit(HitSettingsTabBase + i, rectFrom(tabLeft, 118, tabLeft + 124, 150));
        }
        const int contentTop = 166;
        const int contentBottom = height - 14;
        const int presetColumns = width < 960 ? 4 : 6;
        const int presetRows = (static_cast<int>(kThemes.size()) + presetColumns - 1) / presetColumns;
        const int presetGridHeight = presetRows * 64 + (presetRows - 1) * 8;
        // 预留标题、网格与底部操作区，新增预设时不会与“恢复默认”按钮重叠。
        const int presetCardHeight = 70 + presetGridHeight + 34 + 60;
        const int totalContentHeight = settingsTab_ == 0 ? presetCardHeight * 2 + 188 : (settingsTab_ == 1 ? 384 : 412);
        const int maxScroll = std::max(0, totalContentHeight - (contentBottom - contentTop));
        scrollOffset_ = std::clamp(scrollOffset_, 0, maxScroll);
        target()->PushAxisAlignedClip(D2D1::RectF(0, static_cast<float>(contentTop),
                                                   static_cast<float>(width), static_cast<float>(contentBottom)),
                                      D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        setHitClip(rectFrom(0, contentTop, width, contentBottom));
        int y = contentTop - scrollOffset_;
        // 滚动裁剪由统一视口完成，标题滚出时保留仍在视口内的控件。
        const auto sectionTopVisible = [&](int sectionTop) { return sectionTop < contentBottom; };
        if (settingsTab_ == 0) {
        // 外观 SectionCard 300
        if (sectionTopVisible(y)) {
            renderSettingsCard(left, y, right, y + presetCardHeight, L"外观", L"");
            renderThemeChoices(left + 20, y + 70, true);
            renderWallpaperActions(left + 20, y + presetCardHeight - 60);
        }
        y += presetCardHeight + 22;
        // 窗口效果 SectionCard 144
        if (sectionTopVisible(y)) {
            renderSettingsCard(left, y, right, y + 144, L"窗口效果", L"");
            renderEffectControls(left + 20, y + 70, right - 20);
        }
        y += 166;
        // 应用图标 SectionCard 300
        if (sectionTopVisible(y)) {
            renderSettingsCard(left, y, right, y + presetCardHeight, L"应用图标", L"");
            renderThemeChoices(left + 20, y + 70, false);
            renderIconActions(left + 20, y + presetCardHeight - 60);
        }
        y += presetCardHeight + 22;
        // 接入集成：保留 desktop 版的统一协议说明，并恢复三种真实的一键接入入口。
        }
        if (settingsTab_ == 1) {
        if (sectionTopVisible(y)) {
            renderSettingsCard(left, y, right, y + 384, L"平台接入", L"");
            text(L"连接你的 AI 工具，集中查看任务进展", static_cast<float>(left + 20), static_cast<float>(y + 44),
                 static_cast<float>(right - 100), static_cast<float>(y + 66), 13, p.textPrimary, true);
            // 统一协议 pill
            fillRound(static_cast<float>(right - 80), static_cast<float>(y + 42),
                      static_cast<float>(right - 20), static_cast<float>(y + 68), 13, p.tab);
            strokeRound(static_cast<float>(right - 80), static_cast<float>(y + 42),
                        static_cast<float>(right - 20), static_cast<float>(y + 68), 13, 1, p.success);
            textCentered(L"统一协议", static_cast<float>(right - 80), static_cast<float>(y + 42),
                         static_cast<float>(right - 20), static_cast<float>(y + 68), 11, p.success, true);
            const int actionTop = y + 78;
            const int actionWidth = right - left - 40;
            const int actionLeft = left + 20;
            renderIntegrationAction(actionLeft, actionTop, actionWidth, L"Claude Code",
                                    L"%USERPROFILE%\\.claude\\settings.json",
                                    integrationStatus_.claudeInstalled ? L"已配置" : L"未配置",
                                    integrationStatus_.claudeInstalled ? L"更新接入" : L"一键接入",
                                    HitIntegrationClaude, integrationStatus_.claudeInstalled);
            renderIntegrationAction(actionLeft, actionTop + 96, actionWidth, L"Codex",
                                    L"%USERPROFILE%\\.codex\\config.toml",
                                    integrationStatus_.codexInstalled ? L"已配置" : L"未配置",
                                    integrationStatus_.codexInstalled ? L"更新接入" : L"一键接入",
                                    HitIntegrationCodex, integrationStatus_.codexInstalled);
            renderIntegrationAction(actionLeft, actionTop + 192, actionWidth, L"ChatGPT 网页",
                                    L"Chrome / Edge → 扩展 → 开发者模式 → 加载已解压的扩展",
                                    integrationStatus_.chatgptOnline ? L"已在线" :
                                    (integrationStatus_.chatgptPrepared ? L"待浏览器加载" : L"未准备"),
                                    L"打开目录", HitIntegrationChatGpt, integrationStatus_.chatgptOnline);
            if (!integrationMessage_.empty()) {
                text(integrationMessage_, static_cast<float>(left + 20), static_cast<float>(y + 362),
                     static_cast<float>(right - 20), static_cast<float>(y + 380), 10.5f,
                     integrationMessageIsError_ ? p.danger : p.muted, false, true);
            }
        }
        y += 258;
        }
        if (settingsTab_ == 2) {
        // 数据库位置和数据操作放在同一张卡片中，所有操作按钮保持等宽、同行。
        if (sectionTopVisible(y)) {
            renderSettingsCard(left, y, right, y + 196, L"数据库存储", L"");
            text(L"当前位置",
                 static_cast<float>(left + 20), static_cast<float>(y + 44),
                 static_cast<float>(left + 96), static_cast<float>(y + 64),
                 11, p.muted, true);
            text(shorten(store_.databasePath(), 104),
                 static_cast<float>(left + 96), static_cast<float>(y + 43),
                 static_cast<float>(right - 20), static_cast<float>(y + 65),
                 11, p.textSecondary, false, true);
            if (!pendingDataDirectory_.empty()) {
                text(L"重启后使用", static_cast<float>(left + 20), static_cast<float>(y + 74),
                     static_cast<float>(left + 96), static_cast<float>(y + 94), 11, p.accent, true);
                text(shorten(pendingDataDirectory_ + L"\\data.sqlite", 104),
                     static_cast<float>(left + 96), static_cast<float>(y + 73),
                     static_cast<float>(right - 20), static_cast<float>(y + 95),
                     11, p.accent, false, true);
            } else {
                text(L"选择新文件夹后会复制现有数据，重启生效；旧数据库继续保留。",
                     static_cast<float>(left + 20), static_cast<float>(y + 74),
                     static_cast<float>(right - 20), static_cast<float>(y + 96),
                     11, p.muted);
            }
            const int buttonTop = y + 132;
            const int gap = 10;
            const int buttonLeft = left + 20;
            const int buttonWidth = (right - left - 40 - gap * 3) / 4;
            const auto drawAction = [&](int index, const wchar_t *label, int hitId, bool danger = false) {
                const int x = buttonLeft + index * (buttonWidth + gap);
                const bool hot = hotHit_ == hitId;
                fillRound(static_cast<float>(x), static_cast<float>(buttonTop),
                          static_cast<float>(x + buttonWidth), static_cast<float>(buttonTop + 34), 16,
                          hot ? p.cardHover : p.tab);
                strokeRound(static_cast<float>(x), static_cast<float>(buttonTop),
                            static_cast<float>(x + buttonWidth), static_cast<float>(buttonTop + 34), 16, 1,
                            danger ? p.danger : p.border);
                textCentered(label, static_cast<float>(x), static_cast<float>(buttonTop),
                             static_cast<float>(x + buttonWidth), static_cast<float>(buttonTop + 34),
                             11, danger ? p.danger : p.textSecondary, hot);
                addHit(hitId, rectFrom(x, buttonTop, x + buttonWidth, buttonTop + 34));
            };
            drawAction(0, L"打开数据目录", HitSectionOpenDir);
            drawAction(1, L"更改存储位置", HitChangeDataDirectory);
            drawAction(2, L"全部标记已读", HitSectionMarkRead);
            drawAction(3, L"清空全部", HitSectionClearAll, true);
        }
        y += 218;
        // 开机启动与系统通知均为独立开关，状态和值在同一水平线上。
        if (sectionTopVisible(y)) {
            renderSettingsCard(left, y, right, y + 194, L"启动与通知", L"");
            const bool ready = server_.running();
            const D2D1_COLOR_F runDot = ready ? p.success : p.danger;
            circle(static_cast<float>(left + 24), static_cast<float>(y + 54), 4, runDot);
            text(ready ? L"本地服务运行中" : L"本地服务初始化失败",
                 static_cast<float>(left + 34), static_cast<float>(y + 46),
                 static_cast<float>(right - 20), static_cast<float>(y + 66),
                 13, p.textPrimary, true);
            const auto drawToggleRow = [&](int rowTop, const wchar_t *title, const wchar_t *description,
                                           bool enabled, int hitId) {
                text(title, static_cast<float>(left + 20), static_cast<float>(rowTop),
                     static_cast<float>(right - 170), static_cast<float>(rowTop + 22),
                     12.5f, p.textPrimary, true);
                text(description, static_cast<float>(left + 20), static_cast<float>(rowTop + 23),
                     static_cast<float>(right - 170), static_cast<float>(rowTop + 45),
                     11, p.muted);
                const int toggleLeft = right - 132;
                const int toggleTop = rowTop + 5;
                fillRound(static_cast<float>(toggleLeft), static_cast<float>(toggleTop),
                          static_cast<float>(right - 20), static_cast<float>(toggleTop + 34), 17,
                          enabled ? p.accentSoft : p.tab);
                strokeRound(static_cast<float>(toggleLeft), static_cast<float>(toggleTop),
                            static_cast<float>(right - 20), static_cast<float>(toggleTop + 34), 17, 1,
                            enabled ? p.accentLine : p.border);
                textCentered(enabled ? L"已开启" : L"已关闭",
                             static_cast<float>(toggleLeft), static_cast<float>(toggleTop),
                             static_cast<float>(right - 20), static_cast<float>(toggleTop + 34),
                             11, enabled ? p.accent : p.textSecondary, true);
                addHit(hitId, rectFrom(toggleLeft, toggleTop, right - 20, toggleTop + 34));
            };
            drawToggleRow(y + 78, L"消息通知", L"任务完成、失败或等待输入时显示系统通知",
                          notificationsEnabled_, HitNotifications);
            fillRound(static_cast<float>(left + 20), static_cast<float>(y + 133),
                      static_cast<float>(right - 20), static_cast<float>(y + 134), 1, p.border);
            drawToggleRow(y + 145, L"开机启动", L"登录 Windows 后自动启动并常驻悬浮球",
                          autoStartEnabled_, HitAutoStart);
        }
        }
        clearHitClip();
        target()->PopAxisAlignedClip();
        if (maxScroll > 0) {
            const int trackTop = contentTop, trackBottom = contentBottom, trackHeight = trackBottom - trackTop;
            const int thumbHeight = std::max(34, trackHeight * trackHeight / (trackHeight + maxScroll));
            const int thumbY = trackTop + (trackHeight - thumbHeight) * scrollOffset_ / maxScroll;
            fillRound(static_cast<float>(width - 9), static_cast<float>(trackTop),
                      static_cast<float>(width - 5), static_cast<float>(trackBottom), 2, blend(p.border, 0.45f));
            fillRound(static_cast<float>(width - 10), static_cast<float>(thumbY),
                      static_cast<float>(width - 4), static_cast<float>(thumbY + thumbHeight), 3, blend(p.accent, 0.72f));
        }
    }

    void renderSettingsCard(int left, int top, int right, int bottom, const wchar_t *title, const wchar_t *subtitle) {
        const Palette &p = palette();
        // 亮色设置页使用较轻的玻璃面，保留壁纸细节；文字和边框仍沿用亮色高对比令牌。
        const D2D1_COLOR_F surface = darkMode_ ? p.card : colorFromArgb(0x78f6f9ffu);
        fillRound(static_cast<float>(left), static_cast<float>(top),
                  static_cast<float>(right), static_cast<float>(bottom), 18, surface);
        strokeRound(static_cast<float>(left), static_cast<float>(top),
                    static_cast<float>(right), static_cast<float>(bottom), 18, 1, p.border);
        text(title, static_cast<float>(left + 20), static_cast<float>(top + 16),
             static_cast<float>(right - 20), static_cast<float>(top + 38),
             14, p.textPrimary, true);
        if (subtitle && subtitle[0]) {
            text(subtitle, static_cast<float>(left + 20), static_cast<float>(top + 44),
                 static_cast<float>(right - 20), static_cast<float>(top + 64),
                 12, p.muted);
        }
    }

    void renderEffectControls(int left, int top, int right) {
        const Palette &p = palette();
        const int barLeft = left + 104;
        const int barRight = right - 120;
        const int valueLeft = barRight + 4;
        const int minusX = right - 72;
        const int plusX = right - 36;
        const auto drawButton = [&](int x, int y, const wchar_t *label, int id, bool hot) {
            fillRound(static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + 30),
                      static_cast<float>(y + 28), 14, hot ? p.cardHover : p.tab);
            strokeRound(static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + 30),
                        static_cast<float>(y + 28), 14, 1, p.border);
            textCentered(label, static_cast<float>(x), static_cast<float>(y), static_cast<float>(x + 30),
                         static_cast<float>(y + 28), 15, p.textSecondary, true);
            addHit(id, rectFrom(x, y, x + 30, y + 28));
        };
        const auto drawTrack = [&](int y, float progress) {
            fillRound(static_cast<float>(barLeft), static_cast<float>(y + 9), static_cast<float>(barRight),
                      static_cast<float>(y + 17), 4, blend(p.borderStrong, 0.60f));
            const int fillRight = barLeft + static_cast<int>((barRight - barLeft) * std::clamp(progress, 0.0f, 1.0f));
            if (fillRight > barLeft) fillRound(static_cast<float>(barLeft), static_cast<float>(y + 9),
                                               static_cast<float>(fillRight), static_cast<float>(y + 17), 4, p.accent);
            circle(static_cast<float>(fillRight), static_cast<float>(y + 13), 6, p.accent);
        };
        text(L"窗口透明度", static_cast<float>(left), static_cast<float>(top + 2),
             static_cast<float>(left + 96), static_cast<float>(top + 24), 11, p.textPrimary, true);
        drawTrack(top, (opacityPercent_ - 60) / 40.0f);
        textCentered(std::to_wstring(opacityPercent_) + L"%", static_cast<float>(valueLeft), static_cast<float>(top),
                     static_cast<float>(valueLeft + 44), static_cast<float>(top + 28), 11, p.textSecondary, true);
        drawButton(minusX, top, L"−", HitOpacityDown, hotHit_ == HitOpacityDown);
        drawButton(plusX, top, L"+", HitOpacityUp, hotHit_ == HitOpacityUp);

        const wchar_t *blurLabel = blurLevel_ == 0 ? L"关闭" : (blurLevel_ == 1 ? L"轻度" : L"中度");
        text(L"背景模糊", static_cast<float>(left), static_cast<float>(top + 44),
             static_cast<float>(left + 96), static_cast<float>(top + 66), 11, p.textPrimary, true);
        drawTrack(top + 42, blurLevel_ / 2.0f);
        textCentered(blurLabel, static_cast<float>(valueLeft), static_cast<float>(top + 42),
                     static_cast<float>(valueLeft + 44), static_cast<float>(top + 70), 11, p.textSecondary, true);
        drawButton(minusX, top + 42, L"−", HitBlurDown, hotHit_ == HitBlurDown);
        drawButton(plusX, top + 42, L"+", HitBlurUp, hotHit_ == HitBlurUp);
    }

    void renderThemeChoices(int left, int top, bool wallpaperMode) {
        const Palette &p = palette();
        const int cols = clientWidth() < 960 ? 4 : 6, gap = 8, cellH = 64;
        const int cellW = std::min(160, (clientWidth() - left - 42 - gap * (cols - 1)) / cols);
        const std::wstring selectedTheme = store_.themeId();
        const std::wstring selectedIcon = store_.userIconPath().empty() ? store_.userIconPreset() : L"";
        const std::wstring presets = resourceDirectory() + L"\\presets\\";
        for (size_t i = 0; i < kThemes.size(); ++i) {
            const int row = static_cast<int>(i) / cols;
            const int col = static_cast<int>(i) % cols;
            const int cx = left + col * (cellW + gap);
            const int cy = top + row * (cellH + gap);
            if (hitClipEnabled_ && (cy + cellH < hitClip_.top || cy > hitClip_.bottom)) continue;
            const bool selected = wallpaperMode
                ? (selectedTheme == kThemes[i].id)
                : (selectedIcon == kThemes[i].id);
            fillRound(static_cast<float>(cx), static_cast<float>(cy),
                      static_cast<float>(cx + cellW), static_cast<float>(cy + cellH), 14,
                      selected ? p.accentSoft : p.tab);
            strokeRound(static_cast<float>(cx), static_cast<float>(cy),
                        static_cast<float>(cx + cellW), static_cast<float>(cy + cellH), 14, 1,
                        selected ? p.accentLine : p.border);
            // 壁纸预览使用当前明暗模式对应的壁纸，头像预览继续使用圆形人物图。
            const int prevX = cx + 9, prevY = cy + 9, prevS = 38;
            const std::wstring path = wallpaperMode
                ? resourceDirectory() + L"\\themes\\" + kThemes[i].id + L"\\wallpaper-" +
                    (darkMode_ ? L"dark" : L"light") + L".png"
                : presets + kThemes[i].id + L".png";
            if (wallpaperMode) {
                drawBitmapCover(previewBitmap(path), static_cast<float>(prevX), static_cast<float>(prevY),
                                static_cast<float>(prevS), static_cast<float>(prevS));
            } else {
                drawBitmapCircle(previewBitmap(path), static_cast<float>(prevX), static_cast<float>(prevY),
                                 static_cast<float>(prevS));
            }
            textCentered(kThemes[i].name, static_cast<float>(cx + 50), static_cast<float>(cy),
                 static_cast<float>(cx + cellW - 6), static_cast<float>(cy + cellH),
                 11, p.textPrimary);
            if (wallpaperMode) {
                addHit(HitThemeBase + static_cast<int>(i), rectFrom(cx, cy, cx + cellW, cy + cellH));
            } else {
                addHit(HitIconBase + static_cast<int>(i), rectFrom(cx, cy, cx + cellW, cy + cellH));
            }
        }
    }

    void renderWallpaperActions(int left, int top) {
        const Palette &p = palette();
        // "选择本地壁纸…"
        const int aW = 140, aH = 32, gap = 8;
        const bool hotA = hotHit_ == HitPickWallpaper;
        fillRound(static_cast<float>(left), static_cast<float>(top),
                  static_cast<float>(left + aW), static_cast<float>(top + aH), 14,
                  hotA ? p.cardHover : p.tab);
        strokeRound(static_cast<float>(left), static_cast<float>(top),
                    static_cast<float>(left + aW), static_cast<float>(top + aH), 14, 1, p.border);
        textCentered(L"选择本地壁纸…", static_cast<float>(left), static_cast<float>(top),
                     static_cast<float>(left + aW), static_cast<float>(top + aH),
                     12, p.textSecondary);
        addHit(HitPickWallpaper, rectFrom(left, top, left + aW, top + aH));
        // "恢复默认"
        const int bX = left + aW + gap;
        const bool hotB = hotHit_ == HitClearWallpaper;
        fillRound(static_cast<float>(bX), static_cast<float>(top),
                  static_cast<float>(bX + 100), static_cast<float>(top + aH), 14,
                  hotB ? p.cardHover : p.tab);
        strokeRound(static_cast<float>(bX), static_cast<float>(top),
                    static_cast<float>(bX + 100), static_cast<float>(top + aH), 14, 1, p.border);
        textCentered(L"恢复默认", static_cast<float>(bX), static_cast<float>(top),
                     static_cast<float>(bX + 100), static_cast<float>(top + aH),
                     12, p.textSecondary);
        addHit(HitClearWallpaper, rectFrom(bX, top, bX + 100, top + aH));
        // 状态文字
        const std::wstring status = !store_.wallpaperPath().empty() ? L"已使用本地壁纸" : L"已使用内置主题";
        text(status, static_cast<float>(bX + 110), static_cast<float>(top + 9),
             static_cast<float>(bX + 360), static_cast<float>(top + aH - 6),
             11, p.muted);
    }

    void renderIconActions(int left, int top) {
        const Palette &p = palette();
        const int aW = 140, aH = 32, gap = 8;
        const bool hotA = hotHit_ == HitPickIcon;
        fillRound(static_cast<float>(left), static_cast<float>(top),
                  static_cast<float>(left + aW), static_cast<float>(top + aH), 14,
                  hotA ? p.cardHover : p.tab);
        strokeRound(static_cast<float>(left), static_cast<float>(top),
                    static_cast<float>(left + aW), static_cast<float>(top + aH), 14, 1, p.border);
        textCentered(L"选择本地图片…", static_cast<float>(left), static_cast<float>(top),
                     static_cast<float>(left + aW), static_cast<float>(top + aH),
                     12, p.textSecondary);
        addHit(HitPickIcon, rectFrom(left, top, left + aW, top + aH));
        const int bX = left + aW + gap;
        const bool hotB = hotHit_ == HitClearIcon;
        fillRound(static_cast<float>(bX), static_cast<float>(top),
                  static_cast<float>(bX + 100), static_cast<float>(top + aH), 14,
                  hotB ? p.cardHover : p.tab);
        strokeRound(static_cast<float>(bX), static_cast<float>(top),
                    static_cast<float>(bX + 100), static_cast<float>(top + aH), 14, 1, p.border);
        textCentered(L"恢复默认", static_cast<float>(bX), static_cast<float>(top),
                     static_cast<float>(bX + 100), static_cast<float>(top + aH),
                     12, p.textSecondary);
        addHit(HitClearIcon, rectFrom(bX, top, bX + 100, top + aH));
        const std::wstring status = !store_.userIconPath().empty() ? L"已使用本地图片" : L"已使用内置头像";
        text(status, static_cast<float>(bX + 110), static_cast<float>(top + 9),
             static_cast<float>(bX + 360), static_cast<float>(top + aH - 6),
             11, p.muted);
    }

    void renderOrb(int width, int height) {
        if (!orbExpanded_) {
            // 收起态：圆球窗口
            // 整窗口不画背景（alpha=0），靠 UpdateLayeredWindow 透出桌面
            const Palette &p = palette();
            const float cx = width / 2.0f;
            const float cy = height / 2.0f;
            const float radius = (std::min(width, height) / 2.0f) - 2.0f;
            // tone glow（3 圈）
            const D2D1_COLOR_F tone = palette().accent;
            circleStroke(cx, cy, radius + 1.5f, 8, blend(tone, 0.10f));
            circleStroke(cx, cy, radius + 0.5f, 4, blend(tone, 0.18f));
            // 内圈球体（inset 3）
            const float innerR = radius - 3.0f;
            circle(cx, cy, innerR, palette().card);
            drawBitmapCircle(avatar_.Get(), cx - innerR, cy - innerR, innerR * 2, 1.0f);
            // 1.5px 描边（tone 50% + white 50%）
            circleStroke(cx, cy, innerR, 1.5f, blend(blend(tone, 0.5f), 0.95f));
            // 内高光
            circleStroke(cx, cy - innerR * 0.55f, innerR * 0.7f, 1.0f, blend(color(0xffffff), 0.22f));
            drawOrbBadge(width, height);
            (void)p;
            return;
        }
        // 展开态：240×360
        const Palette &p = palette();
        // 玻璃面板：#ee141a22 + 1px 白色 0.1 + 圆角 20
        const D2D1_COLOR_F panelBg = darkMode_ ? colorFromArgb(0xee141a22u) : colorFromArgb(0xf4ffffffu);
        fillRound(0, 0, static_cast<float>(width), static_cast<float>(height), 20, panelBg);
        strokeRound(0, 0, static_cast<float>(width), static_cast<float>(height), 20, 1,
                    blend(color(0xffffff), 0.10f));
        // 球缩到右下角 36×36
        const int ball = kOrbSize, bInset = 6;
        const RECT ballRect = orbRectLocal();
        circle(static_cast<float>(ballRect.left + ball / 2),
               static_cast<float>(ballRect.top + ball / 2), ball / 2.0f, palette().card);
        drawBitmapCircle(avatar_.Get(), static_cast<float>(ballRect.left + bInset),
                         static_cast<float>(ballRect.top + bInset), ball - 2 * bInset, 1.0f);
        circleStroke(static_cast<float>(ballRect.left + ball / 2),
                     static_cast<float>(ballRect.top + ball / 2), ball / 2.0f, 1.0f, palette().border);
        drawOrbBadge(ballRect.right, ballRect.top + ball);
        // 头部
        const int headingLeft = panelToLeft_ ? 16 : 76;
        text(L"任务概览", headingLeft, 18, headingLeft + 136, 40, 14, p.textPrimary, true);
        text(L"点击消息查看详情", headingLeft, 42, headingLeft + 136, 60, 10, p.muted);
        // 状态 stats（4 pills，22px）
        int sx = 12, sy = 74;
        renderOrbStat(sx, sy, L"执行中", snapshot_.counts.running, color(0x38bdf8)); sx += 72;
        renderOrbStat(sx, sy, L"待输入", snapshot_.counts.needsInput, color(0xf59e0b)); sx += 72;
        renderOrbStat(sx, sy, L"已完成", snapshot_.counts.completedUnread, color(0x22c55e));
        const int taskListTop = 104;
        const int maxList = 4;
        int shown = 0;
        int y = taskListTop;
        for (const auto &task : snapshot_.queue) {
            if (shown++ >= maxList) break;
            const int rowH = 46;
            addHit(HitCardBase + static_cast<int>(task.id), rectFrom(12, y, width - 12, y + rowH), task.id);
            fillRound(12, static_cast<float>(y), static_cast<float>(width - 12), static_cast<float>(y + rowH), 10, p.card);
            strokeRound(12, static_cast<float>(y), static_cast<float>(width - 12), static_cast<float>(y + rowH), 10, 1, p.border);
            // 状态色条 2.5px 居中
            fillRound(16, static_cast<float>(y + 8), 18.5f, static_cast<float>(y + rowH - 8), 1.5f,
                      statusColor(task.status));
            text(sourceLabel(task.source), 26, static_cast<float>(y + 6), 110, static_cast<float>(y + 18),
                 10, sourceColor(task.source), true);
            text(statusLabel(task.status), 110, static_cast<float>(y + 6), static_cast<float>(width - 22),
                 static_cast<float>(y + 18), 10, statusColor(task.status));
            text(shorten(task.title.empty() ? L"未命名任务" : task.title, 30),
                 26, static_cast<float>(y + 22), static_cast<float>(width - 22), static_cast<float>(y + 38),
                 11, p.textPrimary);
            y += rowH + 4;
        }
        if (snapshot_.queue.empty()) {
            text(L"当前没有运行中或待查看的任务", 18, static_cast<float>(y + 8),
                 static_cast<float>(width - 18), static_cast<float>(y + 24), 11, p.muted);
        }
        // 底部按钮
        const int btnY = height - 50;
        const int openW = (width - 36) / 2;
        fillRound(12, static_cast<float>(btnY), static_cast<float>(12 + openW), static_cast<float>(btnY + 32), 14,
                  p.accentSoft);
        strokeRound(12, static_cast<float>(btnY), static_cast<float>(12 + openW), static_cast<float>(btnY + 32), 14, 1, p.accentLine);
        textCentered(L"打开面板", 12, static_cast<float>(btnY), static_cast<float>(12 + openW), static_cast<float>(btnY + 32),
             12, p.accent, true);
        addHit(HitOrb, rectFrom(12, btnY, 12 + openW, btnY + 32));
        const int readX = 12 + openW + 8, readW = width - readX - 12;
        fillRound(static_cast<float>(readX), static_cast<float>(btnY), static_cast<float>(readX + readW), static_cast<float>(btnY + 32), 14, p.tab);
        strokeRound(static_cast<float>(readX), static_cast<float>(btnY), static_cast<float>(readX + readW), static_cast<float>(btnY + 32), 14, 1, p.border);
        textCentered(L"一键已读", static_cast<float>(readX), static_cast<float>(btnY),
             static_cast<float>(readX + readW), static_cast<float>(btnY + 32), 12, p.textSecondary);
        addHit(HitMarkAll, rectFrom(readX, btnY, readX + readW, btnY + 32));
    }

    void renderOrbStat(int x, int y, const wchar_t *label, int value, D2D1_COLOR_F tint) {
        const Palette &p = palette();
        const int w = 70, h = 22;
        fillRound(static_cast<float>(x), static_cast<float>(y),
                  static_cast<float>(x + w), static_cast<float>(y + h), 11,
                  darkMode_ ? colorFromArgb(0x14ffffffu) : colorFromArgb(0x26ffffffu));
        strokeRound(static_cast<float>(x), static_cast<float>(y),
                    static_cast<float>(x + w), static_cast<float>(y + h), 11, 1, p.border);
        circle(static_cast<float>(x + 8), static_cast<float>(y + 11), 3, tint);
        text(std::wstring(label) + L" " + std::to_wstring(value),
             static_cast<float>(x + 16), static_cast<float>(y + 4),
             static_cast<float>(x + w - 4), static_cast<float>(y + h - 2),
             10, p.textPrimary);
    }

    void drawOrbBadge(int right, int bottom) {
        const Palette &p = palette();
        const int count = snapshot_.counts.queue;
        if (count <= 0) return;
        const std::wstring value = count > 99 ? L"99+" : std::to_wstring(count);
        const int badgeW = std::max(18, static_cast<int>(value.size() * 8) + 8);
        const int badgeH = 16;
        const int badgeX = right - badgeW - 6;
        const int badgeY = bottom - kOrbSize + 6;
        fillRound(static_cast<float>(badgeX), static_cast<float>(badgeY),
                  static_cast<float>(badgeX + badgeW), static_cast<float>(badgeY + badgeH),
                  static_cast<float>(badgeH) / 2, p.danger);
        strokeRound(static_cast<float>(badgeX), static_cast<float>(badgeY),
                    static_cast<float>(badgeX + badgeW), static_cast<float>(badgeY + badgeH),
                    static_cast<float>(badgeH) / 2, 1, colorFromArgb(0xcc141a22u));
        textCentered(value, static_cast<float>(badgeX), static_cast<float>(badgeY),
                     static_cast<float>(badgeX + badgeW), static_cast<float>(badgeY + badgeH),
                     9, color(0xffffff), true);
    }

    RECT orbRectLocal() const {
        if (!orbExpanded_) return rectFrom(0, 0, kOrbSize, kOrbSize);
        const int left = panelToLeft_ ? kOrbPanelWidth - kOrbSize - kOrbPanelInset : kOrbPanelInset;
        return rectFrom(left, kOrbPanelInset, left + kOrbSize, kOrbPanelInset + kOrbSize);
    }

    RECT orbRectScreen() const {
        RECT window{};
        GetWindowRect(hwnd_, &window);
        const RECT local = orbRectLocal();
        return rectFrom(window.left + local.left, window.top + local.top, window.left + local.right, window.top + local.bottom);
    }

    void expandOrb() {
        if (!orbMode_ || orbExpanded_ || dragging_) return;
        RECT old{};
        GetWindowRect(hwnd_, &old);
        const POINT center{(old.left + old.right) / 2, (old.top + old.bottom) / 2};
        HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(monitor, &info);
        panelToLeft_ = center.x >= (info.rcWork.left + info.rcWork.right) / 2;
        const int width = kOrbPanelWidth;
        const int height = kOrbPanelHeight;
        int x = panelToLeft_ ? old.left - (width - kOrbSize - kOrbPanelInset) : old.left - kOrbPanelInset;
        // 小球固定在展开面板的上边缘，悬停位置与展开后的小球保持连续。
        int y = old.top - kOrbPanelInset;
        x = std::clamp(x, static_cast<int>(info.rcWork.left + 6), static_cast<int>(info.rcWork.right - width - 6));
        y = std::clamp(y, static_cast<int>(info.rcWork.top + 6), static_cast<int>(info.rcWork.bottom - height - 6));
        orbExpanded_ = true;
        SetWindowRgn(hwnd_, nullptr, TRUE);
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetTimer(hwnd_, kCollapseTimer, 180, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void collapseOrb() {
        // 拖动展开态小球时也必须先切回 52×52 收起态，否则缩小后的窗口仍按
        // 展开面板坐标绘制，整块窗口会变成透明，看起来像小球消失。
        if (!orbMode_ || !orbExpanded_) return;
        RECT current{};
        GetWindowRect(hwnd_, &current);
        const RECT ball = orbRectScreen();
        orbExpanded_ = false;
        const int x = ball.left;
        const int y = ball.top;
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, kOrbSize, kOrbSize, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SetWindowRgn(hwnd_, CreateEllipticRgn(0, 0, kOrbSize, kOrbSize), TRUE);
        KillTimer(hwnd_, kCollapseTimer);
        InvalidateRect(hwnd_, nullptr, FALSE);
        (void)current;
    }

    void enterOrbMode() {
        if (orbMode_) return;
        GetWindowRect(hwnd_, &panelRect_);
        orbMode_ = true;
        orbExpanded_ = false;
        dragging_ = false;
        orbHoverSuppressed_ = false;
        RECT work{};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW | WS_EX_TOPMOST);
        SetWindowPos(hwnd_, HWND_TOPMOST, work.right - kOrbSize - 22, work.top + 22,
                     kOrbSize, kOrbSize, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        SetWindowRgn(hwnd_, CreateEllipticRgn(0, 0, kOrbSize, kOrbSize), TRUE);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void enterPanelMode(bool oppositeSide) {
        if (!orbMode_) return;
        POINT center{0, 0};
        RECT orb{};
        GetWindowRect(hwnd_, &orb);
        center.x = (orb.left + orb.right) / 2;
        center.y = (orb.top + orb.bottom) / 2;
        HMONITOR monitor = MonitorFromPoint(center, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(monitor, &info);
        int x = panelRect_.left;
        int y = panelRect_.top;
        if (oppositeSide) {
            const bool onLeft = center.x < (info.rcWork.left + info.rcWork.right) / 2;
            x = onLeft ? info.rcWork.right - kPanelWidth - 24 : info.rcWork.left + 24;
            y = std::clamp(center.y - kPanelHeight / 2, info.rcWork.top + 12, info.rcWork.bottom - kPanelHeight - 12);
        }
        orbMode_ = false;
        orbExpanded_ = false;
        KillTimer(hwnd_, kCollapseTimer);
        SetWindowRgn(hwnd_, nullptr, TRUE);
        SetWindowLongPtrW(hwnd_, GWL_STYLE, WS_POPUP | WS_THICKFRAME | WS_MAXIMIZEBOX);
        SetWindowLongPtrW(hwnd_, GWL_EXSTYLE, WS_EX_LAYERED | WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW);
        SetWindowPos(hwnd_, HWND_NOTOPMOST, x, y, kPanelWidth, kPanelHeight, SWP_SHOWWINDOW | SWP_FRAMECHANGED);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void openTask(std::int64_t id) {
        HubTask task;
        if (!store_.task(id, task)) return;
        HINSTANCE opened = nullptr;
        if (!task.openUrl.empty()) {
            if (task.openUrl.rfind(L"https://", 0) == 0 || task.openUrl.rfind(L"http://", 0) == 0)
                opened = ShellExecuteW(hwnd_, L"open", task.openUrl.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (!task.projectPath.empty()) {
            const DWORD attributes = GetFileAttributesW(task.projectPath.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY))
                opened = ShellExecuteW(hwnd_, L"open", task.projectPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        if (reinterpret_cast<INT_PTR>(opened) <= 32) {
            MessageBoxW(hwnd_, L"没有可打开的网页或项目目录，或目标已不可用。任务未标记为已读。", L"无法打开任务", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (task.status == L"COMPLETED_UNREAD" || task.status == L"FAILED_UNREAD") store_.setStatus(id, L"VIEWED");
        selectedId_ = id;
        if (orbMode_) enterPanelMode(true);
    }

    void pickImage(bool wallpaper) {
        wchar_t fileName[MAX_PATH * 4]{};
        OPENFILENAMEW dialog{sizeof(dialog)};
        dialog.hwndOwner = hwnd_;
        dialog.lpstrFile = fileName;
        dialog.nMaxFile = static_cast<DWORD>(std::size(fileName));
        dialog.lpstrFilter = L"图片文件\0*.png;*.jpg;*.jpeg;*.bmp;*.webp\0所有文件\0*.*\0";
        dialog.nFilterIndex = 1;
        dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (!GetOpenFileNameW(&dialog)) return;
        if (wallpaper) store_.setWallpaper(fileName);
        else store_.setUserIcon(fileName);
        reloadImages(true);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void changeDataDirectory() {
        const std::wstring selected = chooseDirectory(hwnd_);
        if (selected.empty()) return;
        const std::wstring prompt = L"将当前数据库完整复制到：\n\n" + selected +
            L"\n\n下次启动将使用新位置，旧数据库会保留。是否继续？";
        if (MessageBoxW(hwnd_, prompt.c_str(), L"更改数据库存储位置",
                        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES) return;
        std::wstring error;
        if (!store_.backupToDirectory(selected, error)) {
            MessageBoxW(hwnd_, error.c_str(), L"存储位置未更改", MB_OK | MB_ICONWARNING);
            return;
        }
        if (!writeRegistryString(kApplicationRegistryKey, L"DataDirectory", selected, error)) {
            const std::wstring message = error + L"\n\n数据库副本已保留在所选目录，但当前存储位置未切换。";
            MessageBoxW(hwnd_, message.c_str(), L"存储位置未更改", MB_OK | MB_ICONWARNING);
            return;
        }
        pendingDataDirectory_ = selected;
        MessageBoxW(hwnd_, L"数据库和偏好设置已复制。\n\n请退出并重新打开 AI Task Hub，新存储位置才会生效。旧数据库不会自动删除。",
                    L"迁移完成", MB_OK | MB_ICONINFORMATION);
    }

    void performHit(int id) {
        if (id != HitSearch) searchFocus_ = false;
        if (id == HitCleanupBackdrop) {
            if (cleanupMenuOpen_) cancelClearConfirmation();
            return;
        }
        if (id == HitCleanupQueue) { confirmClear(L"queue", L"个待处理任务", snapshot_.counts.queue); return; }
        if (id == HitCleanupCompleted) {
            confirmClear(L"completed", L"条已完成消息", snapshot_.counts.completedUnread);
            return;
        }
        if (id == HitCleanupHistory) { confirmClear(L"history", L"条历史记录", snapshot_.counts.history); return; }
        if (id == HitCleanupAll) { confirmClear(L"all", L"个任务", snapshot_.counts.total); return; }
        if (id == HitCleanupSources) {
            cleanupSourceMenuOpen_ = !cleanupSourceMenuOpen_;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (id >= HitCleanupSourceBase && id < HitCleanupSourceBase + 4) {
            static constexpr const wchar_t *sources[] = {L"CHATGPT", L"CLAUDE_CODE", L"CODEX", L"OTHER"};
            static constexpr const wchar_t *labels[] = {L"个 GPT 网页任务", L"个 Claude Code 任务", L"个 Codex 任务", L"个其他来源任务"};
            const int index = id - HitCleanupSourceBase;
            confirmClear(L"source:" + std::wstring(sources[index]), labels[index], countForSource(sources[index]));
            return;
        }
        if (id == HitCleanupConfirmYes) { executeClearConfirmation(); return; }
        if (id == HitCleanupConfirmNo) { cancelClearConfirmation(); return; }
        if (id == HitClose) { PostMessageW(hwnd_, WM_CLOSE, 0, 0); return; }
        if (id == HitMinimize || id == HitEnterOrb) { enterOrbMode(); return; }
        if (id == HitCleanupMenu) { showCleanupMenu(); return; }
        if (id == HitMaximize) {
            ShowWindow(hwnd_, IsZoomed(hwnd_) ? SW_RESTORE : SW_MAXIMIZE);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        if (id == HitThemeToggle) { toggleDarkMode(); return; }
        if (id == HitOpacityDown || id == HitOpacityUp) {
            const int delta = id == HitOpacityDown ? -5 : 5;
            opacityPercent_ = std::clamp(opacityPercent_ + delta, 60, 100);
            store_.setWindowOpacityPercent(opacityPercent_);
            return;
        }
        if (id == HitBlurDown || id == HitBlurUp) {
            const int delta = id == HitBlurDown ? -1 : 1;
            blurLevel_ = std::clamp(blurLevel_ + delta, 0, 2);
            store_.setBackgroundBlurLevel(blurLevel_);
            reloadImages(true);
            return;
        }
        if (id >= HitSettingsTabBase && id < HitSettingsTabBase + 3) {
            settingsTab_ = id - HitSettingsTabBase; scrollOffset_ = 0;
            refreshIntegrationStatus();
        }
        else if (id >= HitSourceBase && id < HitSourceBase + 5) {
            static constexpr const wchar_t *sources[] = {L"", L"CHATGPT", L"CLAUDE_CODE", L"CODEX", L"OTHER"};
            sourceFilter_ = sources[id - HitSourceBase]; selectedId_ = 0; scrollOffset_ = 0;
        }
        else if (id >= HitStatusBase && id < HitStatusBase + 5) {
            static constexpr const wchar_t *queue[] = {L"", L"RUNNING", L"NEEDS_INPUT", L"COMPLETED_UNREAD", L"FAILED_UNREAD"};
            static constexpr const wchar_t *history[] = {L"", L"VIEWED", L"IGNORED"};
            const int index = id - HitStatusBase;
            statusFilter_ = page_ == 1 ? history[std::min(index, 2)] : queue[index];
            selectedId_ = 0; scrollOffset_ = 0;
        }
        else if (id == HitNotifications) {
            notificationsEnabled_ = !notificationsEnabled_;
            store_.setNotificationsEnabled(notificationsEnabled_);
        }
        else if (id == HitAutoStart) {
            std::wstring error;
            const bool next = !autoStartEnabled_;
            if (setAutoStartEnabled(next, error)) autoStartEnabled_ = next;
            else MessageBoxW(hwnd_, error.c_str(), L"开机启动设置失败", MB_OK | MB_ICONWARNING);
        }
        else if (id == HitChangeDataDirectory) changeDataDirectory();
        else if (id == HitResetFilters) { search_.clear(); sourceFilter_.clear(); statusFilter_.clear(); scrollOffset_ = 0; }
        else if (id == HitQueue) { page_ = 0; selectedId_ = 0; scrollOffset_ = 0; statusFilter_.clear(); }
        else if (id == HitHistory) { page_ = 1; selectedId_ = 0; scrollOffset_ = 0; statusFilter_.clear(); }
        else if (id == HitSettings) { page_ = 2; selectedId_ = 0; scrollOffset_ = 0; }
        else if (id == HitOrb) { if (orbMode_) enterPanelMode(true); else enterOrbMode(); }
        else if (id == HitMarkAll) store_.markAllViewed();
        else if (id == HitOpenSelected && selectedId_ > 0) openTask(selectedId_);
        else if (id == HitViewSelected && selectedId_ > 0) store_.setStatus(selectedId_, L"VIEWED");
        else if (id == HitCloseDetail) selectedId_ = 0;
        else if (id == HitPickWallpaper) pickImage(true);
        else if (id == HitPickIcon) pickImage(false);
        else if (id == HitClearWallpaper) { store_.clearWallpaper(); reloadImages(true); }
        else if (id == HitClearIcon) { store_.clearUserIcon(); reloadImages(true); }
        else if (id == HitSearch) { searchFocus_ = true; SetFocus(hwnd_); }
        else if (id == HitStatusCycle) {
            static constexpr const wchar_t *filters[] = {L"", L"RUNNING", L"NEEDS_INPUT", L"COMPLETED_UNREAD", L"FAILED_UNREAD"};
            size_t index = 0;
            for (size_t i = 0; i < std::size(filters); ++i) if (statusFilter_ == filters[i]) { index = i; break; }
            statusFilter_ = filters[(index + 1) % std::size(filters)];
            selectedId_ = 0;
        }
        else if (id == HitStatusChip) {
            // 简化：循环状态过滤
            static constexpr const wchar_t *filters[] = {L"", L"RUNNING", L"NEEDS_INPUT", L"COMPLETED_UNREAD", L"FAILED_UNREAD"};
            size_t index = 0;
            for (size_t i = 0; i < std::size(filters); ++i) if (statusFilter_ == filters[i]) { index = i; break; }
            statusFilter_ = filters[(index + 1) % std::size(filters)];
            selectedId_ = 0;
        }
        else if (id == HitSourceChip) {
            static constexpr const wchar_t *sourceIds[] = {L"", L"CHATGPT", L"CLAUDE_CODE", L"CODEX", L"OTHER"};
            size_t index = 0;
            for (size_t i = 0; i < std::size(sourceIds); ++i) if (sourceFilter_ == sourceIds[i]) { index = i; break; }
            sourceFilter_ = sourceIds[(index + 1) % std::size(sourceIds)];
            selectedId_ = 0;
        }
        else if (id == HitCardIgnore && clickedTaskId_ > 0) {
            if (history_) {
                if (MessageBoxW(hwnd_, L"删除这条历史任务及其事件记录？", L"删除任务", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES)
                    store_.remove(clickedTaskId_);
            } else store_.setStatus(clickedTaskId_, L"IGNORED");
        }
        else if (id == HitCardOpen && clickedTaskId_ > 0) openTask(clickedTaskId_);
        else if (id == HitDetailOpen && selectedId_ > 0) openTask(selectedId_);
        else if (id == HitDetailRead && selectedId_ > 0) store_.setStatus(selectedId_, L"VIEWED");
        else if (id == HitDetailDelete && selectedId_ > 0) {
            if (MessageBoxW(hwnd_, L"删除这条任务及其事件记录？此操作无法撤销。", L"删除任务", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES) {
                store_.remove(selectedId_); selectedId_ = 0;
            }
        }
        else if (id == HitSectionOpenDir) ShellExecuteW(hwnd_, L"open", store_.dataDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        else if (id == HitSectionMarkRead) store_.markAllViewed();
        else if (id == HitSectionClearAll) confirmClear(L"all", L"个任务", snapshot_.counts.total);
        else if (id == HitIntegrationClaude) {
            const IntegrationResult result = integrations_.installClaude();
            if (!result.success) MessageBoxW(hwnd_, result.message.c_str(), L"接入未完成", MB_OK | MB_ICONWARNING);
            integrationMessageIsError_ = !result.success;
            integrationMessage_ = shorten(result.message, 116);
            refreshIntegrationStatus();
        }
        else if (id == HitIntegrationCodex) {
            const IntegrationResult result = integrations_.installCodex();
            if (!result.success) MessageBoxW(hwnd_, result.message.c_str(), L"接入未完成", MB_OK | MB_ICONWARNING);
            integrationMessageIsError_ = !result.success;
            integrationMessage_ = shorten(result.message, 116);
            refreshIntegrationStatus();
        }
        else if (id == HitIntegrationChatGpt) {
            const IntegrationResult result = integrations_.prepareChatGptExtension();
            integrationMessageIsError_ = !result.success;
            integrationMessage_ = shorten(result.message, 116);
            if (result.success) {
                const IntegrationStatus info = integrations_.status();
                const HINSTANCE opened = ShellExecuteW(hwnd_, L"open", info.chatgptExtensionDirectory.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                if (reinterpret_cast<INT_PTR>(opened) <= 32) {
                    integrationMessageIsError_ = true;
                    integrationMessage_ = L"扩展目录已准备，但打开目录失败，请手动打开：" + info.chatgptExtensionDirectory;
                }
            }
            refreshIntegrationStatus();
        }
        else if (id >= HitThemeBase && id < HitThemeBase + static_cast<int>(kThemes.size())) {
            if (store_.setTheme(kThemes[static_cast<size_t>(id - HitThemeBase)].id)) reloadImages(true);
        }
        else if (id >= HitIconBase && id < HitIconBase + static_cast<int>(kThemes.size())) {
            if (store_.setUserIconPreset(kThemes[static_cast<size_t>(id - HitIconBase)].id)) reloadImages(true);
        }
        else if (id >= HitCardBase) {
            const std::int64_t taskId = id - HitCardBase;
            selectedId_ = taskId;
            detailScrollOffset_ = 0;
            if (orbMode_ && orbExpanded_) { page_ = 0; enterPanelMode(true); }
        }
        if (selectedId_ > 0) {
            detailEvents_ = store_.events(selectedId_);
            detailReply_ = store_.aiReply(selectedId_);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void beginOrbPointer(POINT screenPoint) {
        // 防止悬停展开留下的收起计时器在拖动途中改写窗口尺寸或位置。
        KillTimer(hwnd_, kCollapseTimer);
        dragging_ = true;
        dragMoved_ = false;
        pressScreen_ = screenPoint;
        if (orbExpanded_) pressWindow_ = orbRectScreen();
        else GetWindowRect(hwnd_, &pressWindow_);
        SetCapture(hwnd_);
    }

    void moveOrbPointer(POINT screenPoint) {
        if (!dragging_) return;
        const int dx = screenPoint.x - pressScreen_.x;
        const int dy = screenPoint.y - pressScreen_.y;
        if (!dragMoved_ && dx * dx + dy * dy >= 25) {
            dragMoved_ = true;
            if (orbExpanded_) collapseOrb();
        }
        if (!dragMoved_) return;
        HMONITOR monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
        MONITORINFO info{sizeof(info)};
        GetMonitorInfoW(monitor, &info);
        const int width = kOrbSize;
        const int height = kOrbSize;
        const int x = std::clamp(pressWindow_.left + dx, info.rcWork.left + 2, info.rcWork.right - width - 2);
        const int y = std::clamp(pressWindow_.top + dy, info.rcWork.top + 2, info.rcWork.bottom - height - 2);
        SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void endOrbPointer() {
        if (!dragging_) return;
        const bool wasDrag = dragMoved_;
        dragging_ = false;
        dragMoved_ = false;
        ReleaseCapture();
        if (!wasDrag) {
            openPanelFromOrb();
        } else {
            // 松开后鼠标仍停在小球上，暂时锁住悬停展开，避免拖动完成瞬间又展开到旁边。
            orbHoverSuppressed_ = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }

    void openPanelFromOrb() { enterPanelMode(true); }

    int countForSource(const wchar_t *source) const {
        int count = 0;
        for (const auto &task : snapshot_.queue) if (task.source == source) ++count;
        for (const auto &task : snapshot_.history) if (task.source == source) ++count;
        return count;
    }

    void confirmClear(const std::wstring &scope, const std::wstring &label, int count) {
        if (count <= 0) return;
        pendingClearScope_ = scope;
        pendingClearLabel_ = label;
        pendingClearCount_ = count;
        cleanupMenuOpen_ = false;
        cleanupSourceMenuOpen_ = false;
        clearConfirmationOpen_ = true;
        SetFocus(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void showCleanupMenu() {
        cleanupMenuOpen_ = true;
        cleanupSourceMenuOpen_ = false;
        SetFocus(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void cancelClearConfirmation() {
        clearConfirmationOpen_ = false;
        cleanupMenuOpen_ = false;
        cleanupSourceMenuOpen_ = false;
        pendingClearScope_.clear();
        pendingClearLabel_.clear();
        pendingClearCount_ = 0;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void executeClearConfirmation() {
        if (!clearConfirmationOpen_) return;
        const std::wstring scope = pendingClearScope_;
        clearConfirmationOpen_ = false;
        cleanupMenuOpen_ = false;
        cleanupSourceMenuOpen_ = false;
        pendingClearScope_.clear();
        pendingClearLabel_.clear();
        pendingClearCount_ = 0;
        store_.clear(scope);
        selectedId_ = 0;
        refreshSnapshot();
    }

    void renderCleanupOverlay(int width, int height) {
        const Palette &p = palette();
        addHit(HitCleanupBackdrop, rectFrom(0, 0, width, height));
        if (clearConfirmationOpen_) {
            fillRect(0, 0, static_cast<float>(width), static_cast<float>(height),
                     darkMode_ ? colorFromArgb(0x66000000u) : colorFromArgb(0x520b1420u));
            const int dialogW = std::min(480, width - 32);
            const int dialogH = 214;
            const int left = (width - dialogW) / 2;
            const int top = std::max(24, (height - dialogH) / 2);
            const int right = left + dialogW;
            fillRound(static_cast<float>(left + 8), static_cast<float>(top + 10),
                      static_cast<float>(right + 8), static_cast<float>(top + dialogH + 10), 18,
                      colorFromArgb(0x30000000u));
            fillRound(static_cast<float>(left), static_cast<float>(top),
                      static_cast<float>(right), static_cast<float>(top + dialogH), 18,
                      darkMode_ ? colorFromArgb(0xf3222731u) : colorFromArgb(0xeaf7faffu));
            strokeRound(static_cast<float>(left), static_cast<float>(top),
                        static_cast<float>(right), static_cast<float>(top + dialogH), 18, 1,
                        darkMode_ ? colorFromArgb(0x42ffffffu) : p.border);

            const float iconX = static_cast<float>(left + 40);
            const float iconY = static_cast<float>(top + 69);
            circle(iconX, iconY, 22, darkMode_ ? colorFromArgb(0x35f59e0bu) : colorFromArgb(0x30c95f43u));
            circleStroke(iconX, iconY, 22, 1, blend(p.warning, 0.65f));
            textCentered(L"!", iconX - 14, iconY - 16, iconX + 14, iconY + 16, 18, p.warning, true);
            text(L"确认清理任务", static_cast<float>(left + 72), static_cast<float>(top + 22),
                 static_cast<float>(right - 24), static_cast<float>(top + 48), 15, p.textPrimary, true);
            const std::wstring prompt = L"确定永久删除 " + std::to_wstring(pendingClearCount_) +
                                        L" " + pendingClearLabel_ + L"吗？";
            text(prompt, static_cast<float>(left + 72), static_cast<float>(top + 58),
                 static_cast<float>(right - 24), static_cast<float>(top + 88), 12.5f, p.textPrimary, false, true);
            text(L"对应的事件流水也会一起删除，此操作不可恢复。",
                 static_cast<float>(left + 72), static_cast<float>(top + 94),
                 static_cast<float>(right - 24), static_cast<float>(top + 122), 11.5f, p.textSecondary, false, true);

            const int buttonH = 36;
            const int buttonW = 112;
            const int buttonGap = 10;
            const int buttonY = top + dialogH - buttonH - 18;
            const int noX = right - 20 - buttonW;
            const int yesX = noX - buttonGap - buttonW;
            const auto drawButton = [&](int x, const wchar_t *label, int id, bool destructive) {
                const bool hot = hotHit_ == id;
                const D2D1_COLOR_F background = destructive
                    ? (darkMode_ ? colorFromArgb(hot ? 0x55ef4444u : 0x36ef4444u)
                                 : colorFromArgb(hot ? 0x45c95f43u : 0x2ac95f43u))
                    : (hot ? p.cardHover : p.card);
                fillRound(static_cast<float>(x), static_cast<float>(buttonY),
                          static_cast<float>(x + buttonW), static_cast<float>(buttonY + buttonH), 14, background);
                strokeRound(static_cast<float>(x), static_cast<float>(buttonY),
                            static_cast<float>(x + buttonW), static_cast<float>(buttonY + buttonH), 14, 1,
                            destructive ? blend(p.danger, 0.65f) : p.border);
                textCentered(label, static_cast<float>(x), static_cast<float>(buttonY),
                             static_cast<float>(x + buttonW), static_cast<float>(buttonY + buttonH),
                             12, destructive ? p.danger : p.textSecondary, true);
                addHit(id, rectFrom(x, buttonY, x + buttonW, buttonY + buttonH));
            };
            drawButton(yesX, L"删除", HitCleanupConfirmYes, true);
            drawButton(noX, L"取消", HitCleanupConfirmNo, false);
            return;
        }

        const int pageRight = width - 22;
        const int clearX = pageRight - 330;
        const int menuW = 270;
        const int menuRight = clearX + 100;
        const int menuLeft = std::clamp(menuRight - menuW, 12, std::max(12, width - menuW - 12));
        const int menuTop = 120;
        const int rowH = 34;
        const int padding = 10;
        const int separator = 12;
        const int queueY = menuTop + padding;
        const int completedY = queueY + rowH;
        const int historyY = completedY + rowH;
        const int sourceY = historyY + rowH + separator;
        const int allY = sourceY + rowH + separator;
        const int menuBottom = allY + rowH + padding;
        fillRound(static_cast<float>(menuLeft + 7), static_cast<float>(menuTop + 9),
                  static_cast<float>(menuRight + 7), static_cast<float>(menuBottom + 9), 14,
                  colorFromArgb(0x30000000u));
        fillRound(static_cast<float>(menuLeft), static_cast<float>(menuTop),
                  static_cast<float>(menuRight), static_cast<float>(menuBottom), 14,
                  darkMode_ ? colorFromArgb(0xf21b2029u) : colorFromArgb(0xeef7f9ffu));
        strokeRound(static_cast<float>(menuLeft), static_cast<float>(menuTop),
                    static_cast<float>(menuRight), static_cast<float>(menuBottom), 14, 1,
                    darkMode_ ? colorFromArgb(0x38ffffffu) : p.border);

        const auto drawRow = [&](int y, const std::wstring &label, int id, int count, bool enabled) {
            const bool hot = enabled && hotHit_ == id;
            if (hot) fillRound(static_cast<float>(menuLeft + 6), static_cast<float>(y),
                               static_cast<float>(menuRight - 6), static_cast<float>(y + rowH), 9,
                               darkMode_ ? colorFromArgb(0x24ffffffu) : colorFromArgb(0x28c95f43u));
            circle(static_cast<float>(menuLeft + 17), static_cast<float>(y + rowH / 2), 3,
                   enabled ? p.accent : blend(p.muted, 0.55f));
            text(label, static_cast<float>(menuLeft + 30), static_cast<float>(y + 8),
                 static_cast<float>(menuRight - 28), static_cast<float>(y + rowH - 7),
                 12, enabled ? p.textPrimary : blend(p.muted, 0.62f), enabled);
            if (enabled) addHit(id, rectFrom(menuLeft + 6, y, menuRight - 6, y + rowH));
        };
        const int queueCount = snapshot_.counts.queue;
        const int completedCount = snapshot_.counts.completedUnread;
        const int historyCount = snapshot_.counts.history;
        const int allCount = snapshot_.counts.total;
        drawRow(queueY, L"删除待处理（" + std::to_wstring(queueCount) + L"）",
                HitCleanupQueue, queueCount, queueCount > 0);
        drawRow(completedY, L"删除已完成（" + std::to_wstring(completedCount) + L"）",
                HitCleanupCompleted, completedCount, completedCount > 0);
        drawRow(historyY, L"删除历史（" + std::to_wstring(historyCount) + L"）",
                HitCleanupHistory, historyCount, historyCount > 0);
        line(static_cast<float>(menuLeft + 14), static_cast<float>(sourceY - 6),
             static_cast<float>(menuRight - 14), static_cast<float>(sourceY - 6), 1, p.border);
        const bool sourceHot = hotHit_ == HitCleanupSources;
        if (sourceHot) fillRound(static_cast<float>(menuLeft + 6), static_cast<float>(sourceY),
                                 static_cast<float>(menuRight - 6), static_cast<float>(sourceY + rowH), 9,
                                 darkMode_ ? colorFromArgb(0x24ffffffu) : colorFromArgb(0x28c95f43u));
        circle(static_cast<float>(menuLeft + 17), static_cast<float>(sourceY + rowH / 2), 3, p.accent);
        text(L"按来源删除", static_cast<float>(menuLeft + 30), static_cast<float>(sourceY + 8),
             static_cast<float>(menuRight - 40), static_cast<float>(sourceY + rowH - 7), 12, p.textPrimary);
        textCentered(L"›", static_cast<float>(menuRight - 32), static_cast<float>(sourceY + 1),
                     static_cast<float>(menuRight - 12), static_cast<float>(sourceY + rowH - 1), 17, p.textSecondary);
        addHit(HitCleanupSources, rectFrom(menuLeft + 6, sourceY, menuRight - 6, sourceY + rowH));
        line(static_cast<float>(menuLeft + 14), static_cast<float>(allY - 6),
             static_cast<float>(menuRight - 14), static_cast<float>(allY - 6), 1, p.border);
        drawRow(allY, L"删除全部任务（" + std::to_wstring(allCount) + L"）",
                HitCleanupAll, allCount, allCount > 0);

        if (!cleanupSourceMenuOpen_) return;
        const int sourceW = 194;
        const int sourceTop = sourceY - 6;
        const bool openLeft = menuLeft >= sourceW + 20;
        const int sourceLeft = openLeft
            ? menuLeft - sourceW - 8
            : std::min(width - sourceW - 12, menuRight + 8);
        const int sourceRight = sourceLeft + sourceW;
        const int sourceBottom = sourceTop + padding + 4 * rowH;
        fillRound(static_cast<float>(sourceLeft + 7), static_cast<float>(sourceTop + 9),
                  static_cast<float>(sourceRight + 7), static_cast<float>(sourceBottom + 9), 14,
                  colorFromArgb(0x30000000u));
        fillRound(static_cast<float>(sourceLeft), static_cast<float>(sourceTop),
                  static_cast<float>(sourceRight), static_cast<float>(sourceBottom), 14,
                  darkMode_ ? colorFromArgb(0xf21b2029u) : colorFromArgb(0xeef7f9ffu));
        strokeRound(static_cast<float>(sourceLeft), static_cast<float>(sourceTop),
                    static_cast<float>(sourceRight), static_cast<float>(sourceBottom), 14, 1,
                    darkMode_ ? colorFromArgb(0x38ffffffu) : p.border);
        const std::array<const wchar_t *, 4> sourceIds{{L"CHATGPT", L"CLAUDE_CODE", L"CODEX", L"OTHER"}};
        for (size_t i = 0; i < sourceIds.size(); ++i) {
            const int count = countForSource(sourceIds[i]);
            const int y = sourceTop + padding + static_cast<int>(i) * rowH;
            const int id = HitCleanupSourceBase + static_cast<int>(i);
            const bool enabled = count > 0;
            const bool hot = enabled && hotHit_ == id;
            if (hot) fillRound(static_cast<float>(sourceLeft + 6), static_cast<float>(y),
                               static_cast<float>(sourceRight - 6), static_cast<float>(y + rowH), 9,
                               darkMode_ ? colorFromArgb(0x24ffffffu) : colorFromArgb(0x28c95f43u));
            circle(static_cast<float>(sourceLeft + 17), static_cast<float>(y + rowH / 2), 3,
                   enabled ? sourceColor(sourceIds[i]) : blend(p.muted, 0.55f));
            const std::wstring label = sourceLabel(std::wstring(sourceIds[i])) + L"（" + std::to_wstring(count) + L"）";
            text(label, static_cast<float>(sourceLeft + 30), static_cast<float>(y + 8),
                 static_cast<float>(sourceRight - 12), static_cast<float>(y + rowH - 7),
                 11.5f, enabled ? p.textPrimary : blend(p.muted, 0.62f), enabled);
            if (enabled) addHit(id, rectFrom(sourceLeft + 6, y, sourceRight - 6, y + rowH));
        }
    }

    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
        Win32App *app = reinterpret_cast<Win32App *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto *create = reinterpret_cast<CREATESTRUCTW *>(lParam);
            app = static_cast<Win32App *>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
            app->hwnd_ = hwnd;
        }
        if (!app) return DefWindowProcW(hwnd, message, wParam, lParam);
        switch (message) {
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd, &paint);
            app->render();
            EndPaint(hwnd, &paint);
            return 0;
        }
        case WM_SIZE:
            // 离屏 DIB 在 render() 内按需重建，尺寸变化只需触发重绘。
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_DISPLAYCHANGE:
            app->reloadImages(true);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_MOUSEMOVE: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (app->orbMode_) {
                TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
                TrackMouseEvent(&track);
                if (app->dragging_) {
                    // 窗口会在拖动过程中不断移动，WM_MOUSEMOVE 的 lParam 可能仍是旧窗口
                    // 坐标；直接读取屏幕坐标可避免一次拖动被重复偏移到屏幕外。
                    POINT screen{};
                    GetCursorPos(&screen);
                    app->moveOrbPointer(screen);
                }
                else {
                    const RECT orb = app->orbRectLocal();
                    if (PtInRect(&orb, point)) {
                        if (app->orbHoverSuppressed_) return 0;
                        KillTimer(hwnd, kCollapseTimer);
                        app->expandOrb();
                    }
                    else {
                        app->orbHoverSuppressed_ = false;
                        if (app->orbExpanded_) SetTimer(hwnd, kCollapseTimer, 180, nullptr);
                    }
                }
                return 0;
            }
            const int hot = app->hitAt(point);
            const auto task = app->taskAt(point);
            if (hot != app->hotHit_ || task != app->hotTaskId_) {
                app->hotHit_ = hot; app->hotTaskId_ = task;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            if (app->orbMode_) {
                app->orbHoverSuppressed_ = false;
                if (app->orbExpanded_) SetTimer(hwnd, kCollapseTimer, 180, nullptr);
            }
            app->hotHit_ = HitNone;
            app->hotTaskId_ = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            app->pressedHit_ = app->hitAt(point);
            app->pressedTaskId_ = app->taskAt(point);
            app->searchFocus_ = app->pressedHit_ == HitSearch;
            if (app->orbMode_) {
                const RECT orb = app->orbRectLocal();
                if (PtInRect(&orb, point)) {
                    POINT screen{};
                    if (!GetCursorPos(&screen)) {
                        screen = point;
                        ClientToScreen(hwnd, &screen);
                    }
                    app->beginOrbPointer(screen);
                    return 0;
                }
                return 0;
            }
            const int hit = app->hitAt(point);
            // 顶部拖动区：左侧 (0,0)..(width-280,46) 视为标题栏
            const int dragRight = app->clientWidth() - 280;
            if (hit == HitNone && point.y < 46 && point.x < dragRight && point.x > 180) {
                ReleaseCapture();
                SendMessageW(hwnd, WM_NCLBUTTONDOWN, HTCAPTION, 0);
                return 0;
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (app->orbMode_ && app->dragging_) {
                app->pressedHit_ = HitNone;
                app->endOrbPointer(); return 0;
            }
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            const int hit = app->hitAt(point);
            if (hit == app->pressedHit_ && app->taskAt(point) == app->pressedTaskId_) {
                app->clickedTaskId_ = app->pressedTaskId_;
                app->performHit(hit);
            }
            app->pressedHit_ = HitNone;
            return 0;
        }
        case WM_CAPTURECHANGED:
        case WM_CANCELMODE:
            app->dragging_ = false;
            app->pressedHit_ = HitNone;
            return 0;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT) {
                POINT point{}; GetCursorPos(&point); ScreenToClient(hwnd, &point);
                const int hit = app->hitAt(point);
                SetCursor(LoadCursorW(nullptr, hit == HitSearch ? IDC_IBEAM : (hit != HitNone ? IDC_HAND : IDC_ARROW)));
                return TRUE;
            }
            break;
        case WM_CHAR:
            if (!app->orbMode_ && app->searchFocus_) {
                if (wParam >= 0x20 && wParam != 0x7f && wParam != L'\r') {
                    app->search_.push_back(static_cast<wchar_t>(wParam));
                    app->selectedId_ = 0;
                    app->scrollOffset_ = 0;
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (app->clearConfirmationOpen_) {
                if (wParam == VK_RETURN) app->executeClearConfirmation();
                else if (wParam == VK_ESCAPE) app->cancelClearConfirmation();
                return 0;
            }
            if (app->cleanupMenuOpen_ && wParam == VK_ESCAPE) {
                app->cancelClearConfirmation();
                return 0;
            }
            if ((GetKeyState(VK_CONTROL) & 0x8000) && (wParam == 'K' || wParam == 'F')) {
                if (app->orbMode_) app->enterPanelMode(true);
                if (app->page_ == 2) app->page_ = 0;
                app->searchFocus_ = true;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (!app->orbMode_ && app->searchFocus_) {
                if ((GetKeyState(VK_CONTROL) & 0x8000) && wParam == 'V') {
                    if (OpenClipboard(hwnd)) {
                        HANDLE data = GetClipboardData(CF_UNICODETEXT);
                        if (data) {
                            const auto *value = static_cast<const wchar_t *>(GlobalLock(data));
                            if (value) {
                                app->search_.append(value, std::min<size_t>(wcslen(value), 1024));
                                GlobalUnlock(data);
                            }
                        }
                        CloseClipboard();
                    }
                    app->selectedId_ = 0; app->scrollOffset_ = 0;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (wParam == VK_BACK) {
                    if (!app->search_.empty()) app->search_.pop_back();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
                if (wParam == VK_ESCAPE) {
                    app->searchFocus_ = false;
                    app->search_.clear();
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            if (wParam == VK_ESCAPE && !app->orbMode_) {
                app->selectedId_ = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_KILLFOCUS:
            app->searchFocus_ = false;
            break;
        case WM_MOUSEWHEEL:
            if (!app->orbMode_) {
                POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                ScreenToClient(hwnd, &point);
                int &offset = app->selectedId_ > 0 && PtInRect(&app->detailViewport_, point)
                    ? app->detailScrollOffset_ : app->scrollOffset_;
                offset = std::max(0, offset - GET_WHEEL_DELTA_WPARAM(wParam) / 2);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_NCHITTEST: {
            if (app->orbMode_) return HTCLIENT;
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT window{};
            GetWindowRect(hwnd, &window);
            const int border = 8;
            const bool left = point.x < window.left + border;
            const bool right = point.x >= window.right - border;
            const bool top = point.y < window.top + border;
            const bool bottom = point.y >= window.bottom - border;
            if (top && left) return HTTOPLEFT;
            if (top && right) return HTTOPRIGHT;
            if (bottom && left) return HTBOTTOMLEFT;
            if (bottom && right) return HTBOTTOMRIGHT;
            if (left) return HTLEFT;
            if (right) return HTRIGHT;
            if (top) return HTTOP;
            if (bottom) return HTBOTTOM;
            const POINT local{point.x - window.left, point.y - window.top};
            // 顶部 drag strip：跳过侧栏 (180) 和右上窗口控件区 (280)
            const int dragRight = app->clientWidth() - 280;
            if (local.y < 46 && local.x > 180 && local.x < dragRight) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_GETMINMAXINFO: {
            auto *info = reinterpret_cast<MINMAXINFO *>(lParam);
            info->ptMinTrackSize.x = 860;
            info->ptMinTrackSize.y = 540;
            return 0;
        }
        case WM_TIMER:
            if (wParam == kStartupActivateTimer) {
                KillTimer(hwnd, kStartupActivateTimer);
                app->activateAtStartup();
            }
            else if (wParam == kCollapseTimer && app->orbMode_ && app->orbExpanded_ && !app->dragging_) {
                POINT cursor{};
                GetCursorPos(&cursor);
                RECT window{};
                GetWindowRect(hwnd, &window);
                if (!PtInRect(&window, cursor)) app->collapseOrb();
            }
            else if (wParam == kRefreshTimer) {
                // 任务由事件消息刷新；只在可见的接入页检查扩展心跳。
                if (!app->orbMode_ && app->page_ == 2 && app->settingsTab_ == 1) {
                    app->refreshIntegrationStatus();
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
            }
            return 0;
        case kRefreshMessage:
            app->refreshSnapshot();
            return 0;
        case kTrayMessage: {
            // NOTIFYICON_VERSION_4 将托盘事件放在 lParam 低字中，不能直接比较整个 LPARAM。
            const UINT trayEvent = LOWORD(static_cast<ULONG_PTR>(lParam));
            if (trayEvent == WM_LBUTTONUP || trayEvent == WM_LBUTTONDBLCLK) app->enterPanelMode(false);
            else if (trayEvent == NIN_BALLOONUSERCLICK) app->enterPanelMode(false);
            else if (trayEvent == WM_RBUTTONUP || trayEvent == WM_CONTEXTMENU) {
                POINT cursor{};
                GetCursorPos(&cursor);
                HMENU menu = CreatePopupMenu();
                AppendMenuW(menu, MF_STRING, 1, L"打开任务中心");
                AppendMenuW(menu, MF_STRING, 2, L"收起为悬浮球");
                AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                AppendMenuW(menu, MF_STRING, 3, L"退出");
                SetForegroundWindow(hwnd);
                const int command = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_NONOTIFY, cursor.x, cursor.y, 0, hwnd, nullptr);
                DestroyMenu(menu);
                if (command == 1) app->enterPanelMode(false);
                else if (command == 2) app->enterOrbMode();
                else if (command == 3) PostMessageW(hwnd, WM_CLOSE, 0, 0);
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kCollapseTimer);
            KillTimer(hwnd, kRefreshTimer);
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcW(hwnd, message, wParam, lParam);
        }
        return DefWindowProcW(hwnd, message, wParam, lParam);
    }

    int clientWidth() const {
        if (backing_.width > 0) return backing_.width;
        RECT window{};
        GetWindowRect(hwnd_, &window);
        return window.right - window.left;
    }

    HINSTANCE instance_ = nullptr;
    HWND hwnd_ = nullptr;
    NOTIFYICONDATAW tray_{};
    TaskStore store_;
    IntegrationManager integrations_;
    HttpServer server_;
    HubSnapshot snapshot_;
    ComPtr<ID2D1Factory> d2dFactory_;
    ComPtr<IDWriteFactory> writeFactory_;
    ComPtr<IWICImagingFactory> wicFactory_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<ID2D1Bitmap> wallpaper_;
    ComPtr<ID2D1Bitmap> avatar_;
    std::map<std::wstring, ComPtr<ID2D1Bitmap>> previewBitmaps_;
    std::map<int, ComPtr<IDWriteTextFormat>> formats_;
    MemoryBacking backing_{};
    std::wstring loadedWallpaper_;
    std::wstring loadedAvatar_;
    std::wstring serverWarning_;
    IntegrationStatus integrationStatus_{};
    std::wstring integrationMessage_;
    std::wstring sourceFilter_;
    std::wstring statusFilter_;
    std::wstring search_;
    std::vector<HubEvent> detailEvents_;
    std::wstring detailReply_;
    std::vector<HitRegion> hits_;
    RECT hitClip_{};
    RECT detailViewport_{};
    RECT panelRect_{0, 0, kPanelWidth, kPanelHeight};
    bool snapshotInitialized_ = false;
    POINT pressScreen_{};
    RECT pressWindow_{};
    int page_ = 0;
    int selectedId_ = 0;
    int hotHit_ = HitNone;
    int pressedHit_ = HitNone;
    int settingsTab_ = 0;
    std::int64_t hotTaskId_ = 0;
    std::int64_t clickedTaskId_ = 0;
    std::int64_t pressedTaskId_ = 0;
    int scrollOffset_ = 0;
    int detailScrollOffset_ = 0;
    bool orbMode_ = false;
    bool orbExpanded_ = false;
    bool panelToLeft_ = false;
    bool orbHoverSuppressed_ = false;
    bool dragging_ = false;
    bool dragMoved_ = false;
    bool searchFocus_ = false;
    bool darkMode_ = true;
    bool notificationsEnabled_ = true;
    bool autoStartEnabled_ = false;
    bool history_ = false;
    bool hitClipEnabled_ = false;
    bool integrationMessageIsError_ = false;
    bool cleanupMenuOpen_ = false;
    bool cleanupSourceMenuOpen_ = false;
    bool clearConfirmationOpen_ = false;
    std::wstring pendingDataDirectory_;
    std::wstring pendingClearScope_;
    std::wstring pendingClearLabel_;
    int pendingClearCount_ = 0;
    int opacityPercent_ = 100;
    int blurLevel_ = 0;
};

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&common);
    Win32App app;
    const int result = app.run(instance);
    CoUninitialize();
    return result;
}
