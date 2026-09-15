#include "../src/integration_manager.cpp"
#include "task_store.h"
#include <iostream>
#include <stdexcept>

namespace {
int checks = 0;
void check(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
    ++checks;
}
void put(const Path &path, const std::string &value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file << value;
    if (!file) throw std::runtime_error("fixture write failed");
}
std::string read(const Path &path) {
    std::string value;
    check(readUtf8(path.wstring(), value), "fixture read failed");
    return value;
}
jsonlite::Value json(const std::string &input) {
    jsonlite::Value value;
    std::string error;
    check(jsonlite::parseUtf8(input, value, error), "JSON parse failed");
    return value;
}
}

int wmain(int argc, wchar_t **argv) {
    try {
        check(argc == 2, "supply isolated fixture directory");
        const Path root = std::filesystem::absolute(argv[1]);
        check(!std::filesystem::exists(root), "fixture directory must be new");
        std::filesystem::create_directories(root);
        // 只生成测试目录，不接触真实用户目录，也不执行任何外部适配器。
        wchar_t executable[32768]{};
        GetModuleFileNameW(nullptr, executable, 32768);
        SetEnvironmentVariableW(L"AIHUB_PYTHON", executable);
        const auto profile = root / L"profile", data = root / L"data", assets = root / L"assets";
        put(assets / L"claude-code/claude_adapter.py", "# inert fixture\n");
        put(assets / L"codex/notify_chain.py", "# inert fixture\n");
        put(assets / L"codex/event_converter.py", "# inert fixture\n");
        for (const auto *name : {L"manifest.json", L"background.js", L"content.js", L"README.md"})
            put(assets / L"chatgpt-extension" / name, "{}");
        IntegrationManager manager(profile.wstring(), data.wstring(), assets.wstring());

        auto original = json("{\"ts\":1788714001,\"emoji\":\"\\ud83d\\ude80\",\"中文\":\"保留\"}");
        auto roundtrip = json(jsonlite::stringifyUtf8(original));
        check(roundtrip.get(L"ts")->number() == 1788714001, "timestamp precision");
        check(roundtrip.get(L"emoji")->string() == original.get(L"emoji")->string(), "emoji roundtrip");
        jsonlite::Value invalid; std::string error;
        check(!jsonlite::parseUtf8("{\"a\":1,\"a\":2}", invalid, error), "reject duplicate keys");

        const auto claudePath = profile / L".claude/settings.json";
        const std::string claude = "{\"custom\":1788714001,\"emoji\":\"\\ud83d\\ude80\",\"hooks\":{\"Stop\":[{\"matcher\":\"claude_adapter.py\",\"hooks\":[{\"type\":\"command\",\"command\":\"echo keep-me\"}]}]}}";
        put(claudePath, claude);
        const auto cache = data / L"adapters/claude-code/session_titles.json";
        put(cache, "user cache");
        check(manager.installClaude().success, "Claude installation");
        const auto installedClaude = read(claudePath);
        check(read(cache) == "user cache", "preserve session cache");
        check(installedClaude.find("echo keep-me") != std::string::npos, "preserve other hooks");
        check(installedClaude.find("\"matcher\":\"claude_adapter.py\"") != std::string::npos, "preserve non-command fields");
        check(!manager.installClaude().changed && read(claudePath) == installedClaude, "Claude idempotence");
        check(manager.status().claudeInstalled, "Claude complete hooks detected");
        bool backupFound = false;
        for (const auto &entry : std::filesystem::directory_iterator(claudePath.parent_path()))
            if (entry.path().extension() == L".bak") backupFound = read(entry.path()) == claude;
        check(backupFound, "original config backup");
        put(claudePath, "{broken");
        check(!manager.installClaude().success && read(claudePath) == "{broken", "invalid Claude config preserved");
        check(!manager.status().claudeInstalled, "invalid Claude config not configured");

        const auto codexPath = profile / L".codex/config.toml";
        const std::string nested = "model = 'test'\n[profiles.custom]\nnotify = ['leave-this-alone']\n";
        put(codexPath, nested);
        check(manager.installCodex().success, "Codex insert");
        const auto installedCodex = read(codexPath);
        check(installedCodex.rfind("notify = [", 0) == 0, "notify inserted at root");
        check(installedCodex.find(nested) != std::string::npos, "nested table preserved");
        check(manager.status().codexInstalled, "Codex root command detected");
        check(!manager.installCodex().changed && read(codexPath) == installedCodex, "Codex idempotence");
        put(codexPath, "notify = [\n 'old.exe', # keep ] bracket in comment\n 'arg',\n] # trailing note\n[model_providers.custom]\nname = 'keep'\n");
        check(manager.installCodex().success, "multiline notify array");
        check(read(codexPath).find("# trailing note") != std::string::npos, "trailing comment preserved");
        auto forward = json(read(data / L"adapters/codex/forward_target.json"));
        check(forward.get(L"command")->array()[0].string() == L"old.exe", "old notifier forwarding");
        for (const auto *bad : {"notify = [,'bad']\n", "instructions = '''\nnotify = ['fake']\n'''\n", "notify = ['one']\nnotify = ['two']\n"}) {
            put(codexPath, bad);
            check(!manager.installCodex().success && read(codexPath) == bad, "unsupported TOML preserved");
        }
        check(manager.prepareChatGptExtension().success, "extension prepare");
        check(!manager.status().chatgptOnline, "prepared is not online");
        check(manager.recordChatGptHeartbeat("{\"version\":\"test\"}"), "heartbeat write");
        check(manager.status().chatgptOnline, "heartbeat online");
        put(data / L"chatgpt_heartbeat.json", "{\"ts\":9999999999}");
        check(!manager.status().chatgptOnline, "future heartbeat rejected");

        const auto database = root / L"database";
        const auto relocatedDatabase = root / L"relocated-database";
        {
            TaskStore store(database.wstring());
            check(store.ready() && store.snapshot().counts.total == 0, "isolated database");
            const auto first = store.ingest(json("{\"source\":\"CODEX\",\"externalTaskId\":\"one\",\"eventType\":\"TASK_COMPLETED\",\"title\":\"中文任务\"}"));
            const auto second = store.ingest(json("{\"source\":\"CHATGPT\",\"externalTaskId\":\"two\",\"eventType\":\"TASK_STARTED\",\"title\":\"Running\"}"));
            check(first > 0 && second > 0, "event ingestion");
            check(store.tasks(L"queue", L"", L"CODEX", L"", 20, 0).size() == 1, "source filtering");
            check(store.markAllViewed() == 1 && store.snapshot().counts.running == 1, "mark read preserves running tasks");
            check(store.clear(L"source:") == 0, "empty source must not clear all");
            check(store.clear(L"history:source:CODEX") == 1, "scoped history deletion");
            check(store.events(first).empty() && store.snapshot().counts.total == 1, "delete cascades only selected task");
            check(store.clear(L"nonsense") == 0, "unknown delete scope rejected");
            check(store.setStatus(second, L"IGNORED"), "ignore task");
            const auto completed = store.ingest(json("{\"source\":\"CODEX\",\"externalTaskId\":\"three\",\"eventType\":\"TASK_COMPLETED\",\"title\":\"待删除完成消息\"}"));
            check(completed > 0 && store.clear(L"completed") == 1, "completed-only deletion");
            check(store.events(completed).empty() && store.snapshot().counts.total == 1, "completed deletion cascades events");
            check(store.setTheme(L"ayaka-kamisato") && store.themeId() == L"ayaka-kamisato", "new wallpaper preset persists");
            check(store.setUserIconPreset(L"shorekeeper") && store.userIconPreset() == L"shorekeeper",
                  "new icon preset persists");
            store.setDarkMode(false); store.setNotificationsEnabled(false);
            std::wstring migrationError;
            check(store.backupToDirectory(relocatedDatabase.wstring(), migrationError), "database snapshot migration");
            check(std::filesystem::exists(database / L"data.sqlite"), "source database retained");
            check(std::filesystem::exists(relocatedDatabase / L"data.sqlite"), "destination database created");
            check(std::filesystem::exists(relocatedDatabase / L"AI Task Hub.ini"), "settings copied with database");
            check(!store.backupToDirectory(relocatedDatabase.wstring(), migrationError), "existing database is not overwritten");
        }
        {
            TaskStore store(database.wstring());
            check(!store.darkMode() && !store.notificationsEnabled(), "settings persist");
            check(store.snapshot().counts.ignored == 1, "task status persists");
        }
        {
            TaskStore relocated(relocatedDatabase.wstring());
            check(relocated.ready(), "relocated database opens");
            check(!relocated.darkMode() && !relocated.notificationsEnabled(), "relocated settings persist");
            check(relocated.snapshot().counts.ignored == 1, "relocated task data persists");
        }
        std::cout << "PASS " << checks << " checks; isolated fixtures retained for inspection\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
