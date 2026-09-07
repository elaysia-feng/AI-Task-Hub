#pragma once

#include <string>
#include <mutex>

struct IntegrationStatus {
    bool claudeInstalled = false;
    bool codexInstalled = false;
    bool chatgptPrepared = false;
    bool chatgptOnline = false;
    bool codexRunning = false;
    std::wstring claudeSettingsPath;
    std::wstring codexConfigPath;
    std::wstring chatgptExtensionDirectory;
    std::wstring pythonCommand;
};

struct IntegrationResult {
    bool success = false;
    bool changed = false;
    bool forwardTarget = false;
    std::wstring message;
};

// 管理三类外部适配器的本地配置。配置文件只在用户点击“一键接入”后修改，
// 适配器脚本物化到可写的 %APPDATA%/AI Task Hub，避免指向临时目录或 exe 旁的只读目录。
class IntegrationManager final {
public:
    // 可指定隔离目录进行回归验证；桌面运行时使用系统用户目录。
    IntegrationManager(std::wstring profile = {}, std::wstring data = {}, std::wstring adapters = {});

    IntegrationStatus status() const;
    IntegrationResult installClaude();
    IntegrationResult installCodex();
    IntegrationResult prepareChatGptExtension();

    // ChatGPT 扩展每 5 分钟调用一次；返回 false 表示请求载荷无效或无法落盘。
    bool recordChatGptHeartbeat(const std::string &body);

    std::string statusJson() const;

private:
    std::wstring executableDirectory() const;
    std::wstring userProfileDirectory() const;
    std::wstring userDataDirectory() const;
    std::wstring resourceAdaptersDirectory() const;
    std::wstring claudeAdapterDirectory() const;
    std::wstring codexAdapterDirectory() const;
    std::wstring chatgptExtensionDirectory() const;
    std::wstring heartbeatPath() const;
    std::wstring findPython() const;

    bool materializeDirectory(const std::wstring &source, const std::wstring &target,
                              const std::wstring &requiredFiles, std::wstring &error) const;

    std::wstring executableDirectory_;
    std::wstring profileOverride_;
    std::wstring dataOverride_;
    std::wstring adaptersOverride_;
    std::mutex writeMutex_;
};
