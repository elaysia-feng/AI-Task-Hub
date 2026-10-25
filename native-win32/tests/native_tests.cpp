#include "../src/integration_manager.cpp"
#include "../src/update_checker.cpp"
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
void putBytes(const Path &path, const std::string &value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream file(path, std::ios::binary);
    file.write(value.data(), static_cast<std::streamsize>(value.size()));
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
        SetEnvironmentVariableW(L"PROGRAMDATA", (root / L"program-data").c_str());
        put(assets / L"claude-code/claude_adapter.py", "# inert fixture\n");
        put(assets / L"codex/notify_chain.py", "# inert fixture\n");
        put(assets / L"codex/event_converter.py", "# inert fixture\n");
        put(assets / L"codex/prompt_hook.py", "# inert fixture\n");
        for (const auto *name : {L"manifest.json", L"background.js", L"content.js", L"README.md"})
            put(assets / L"chatgpt-extension" / name, "{}");
        put(assets / L"zcode/zcode_adapter.py", "# inert fixture\n");
        put(assets / L"minimax-code/minimax_adapter.py", "# inert fixture\n");
        put(assets / L"minimax-code/plugin.json", "{\"name\":\"ai-task-hub-bridge\"}");
        put(assets / L"minimax-code/hooks.json",
            "{\n  \"hooks\": {\n    \"UserPromptSubmit\": [\n      {\n        \"hooks\": [\n          {\n"
            "            \"type\": \"command\",\n            \"command\": \"\\\"{{PYTHON}}\\\" \\\"{{SCRIPT}}\\\"\",\n"
            "            \"timeout\": 3\n          }\n        ]\n      }\n    ]\n  }\n}\n");
        putBytes(assets / L"minimax-code/icon.png", std::string("\x89PNG\r\n\x1a\n", 8) + "inert-icon");
        // MiniMax Code 的数据目录必须先指向隔离目录，真实用户目录永远不会被写入。
        const auto minimaxData = root / L"minimax-data";
        std::filesystem::create_directories(minimaxData);
        SetEnvironmentVariableW(L"MINIMAX_DATA_DIR", minimaxData.c_str());
        SetEnvironmentVariableW(L"MAVIS_DATA_DIR", minimaxData.c_str());
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

        const std::wstring pythonPath(executable);
        const auto zcodeCli = profile / L".zcode/cli";
        std::filesystem::create_directories(zcodeCli);
        const auto zcodePath = zcodeCli / L"config.json";
        const std::string zcodeSeed =
            "{\"theme\":\"dark\",\"hooks\":{\"events\":{\"Stop\":[{\"hooks\":[{\"type\":\"process\","
            "\"command\":\"other-tool.exe\",\"args\":[\"keep-me\"]}]}]}}}";
        put(zcodePath, zcodeSeed);
        const auto zcodeInstall = manager.installZcode();
        check(zcodeInstall.success && zcodeInstall.changed, "ZCode installation");
        const std::string installedZcodeText = read(zcodePath);
        const auto installedZcode = json(installedZcodeText);
        check(installedZcode.get(L"theme")->string() == L"dark", "ZCode unrelated keys preserved");
        const auto *zcodeHooks = installedZcode.get(L"hooks");
        check(zcodeHooks && zcodeHooks->isObject(), "ZCode hooks object");
        check(zcodeHooks->get(L"enabled")->boolean(), "ZCode hooks enabled");
        const auto *zcodeEvents = zcodeHooks->get(L"events");
        check(zcodeEvents && zcodeEvents->isObject(), "ZCode events object");
        for (const auto *event : {L"UserPromptSubmit", L"PermissionRequest", L"Stop"}) {
            const auto *entries = zcodeEvents->get(event);
            check(entries && entries->isArray() && !entries->array().empty(), "ZCode event registered");
            bool ours = false;
            for (const auto &entry : entries->array()) {
                // ZCode 的结构是严格的：条目只允许 matcher/hooks，钩子只允许 type/command/args/enabled/timeoutMs。
                check(entry.object().size() == 1 && entry.get(L"hooks") != nullptr, "ZCode entry keeps strict schema");
                const auto *hookList = entry.get(L"hooks");
                check(hookList && hookList->isArray(), "ZCode hook list");
                for (const auto &hook : hookList->array()) {
                    const auto *command = hook.get(L"command");
                    const auto *args = hook.get(L"args");
                    if (!command || !command->isString() || !args || !args->isArray()) continue;
                    const bool oursCommand = command->string() == pythonPath;
                    const bool oursArgs = !args->array().empty() && args->array()[0].isString() &&
                                         args->array()[0].string().find(L"zcode_adapter.py") != std::wstring::npos;
                    if (!oursCommand && !oursArgs) continue;
                    check(hook.object().size() == 4, "ZCode hook keeps strict schema");
                    check(hook.get(L"type")->string() == L"process", "ZCode hook type");
                    check(hook.get(L"timeoutMs")->number() == 8000, "ZCode hook timeout in milliseconds");
                    check(oursCommand && oursArgs, "ZCode hook points at python and adapter");
                    ours = true;
                }
            }
            check(ours, "ZCode event carries our hook");
        }
        const auto &stopEntries = zcodeEvents->get(L"Stop")->array();
        check(stopEntries.size() == 2, "ZCode keeps third party hooks without duplicating our entry");
        bool thirdPartyKept = false;
        for (const auto &entry : stopEntries)
            for (const auto &hook : entry.get(L"hooks")->array())
                if (hook.get(L"command")->string() == L"other-tool.exe") thirdPartyKept = true;
        check(thirdPartyKept, "ZCode third party hook preserved");
        check(!manager.installZcode().changed && read(zcodePath) == installedZcodeText, "ZCode idempotence");
        check(manager.status().zcodeInstalled && manager.status().zcodeHooksEnabled, "ZCode status detected");
        put(zcodePath, "{broken");
        check(!manager.installZcode().success && read(zcodePath) == "{broken", "invalid ZCode config preserved");
        put(zcodePath, "{\"hooks\":{\"enabled\":false,\"events\":{}}}");
        check(manager.installZcode().success, "ZCode install keeps user disabled hooks");
        check(json(read(zcodePath)).get(L"hooks")->get(L"enabled")->boolean() == false,
              "ZCode preserves hooks.enabled=false");
        // 钩子已登记但全局开关关闭：仍算「已接入」，另用 hooksEnabled 单独提示。
        const auto disabledStatus = manager.status();
        check(disabledStatus.zcodeInstalled && !disabledStatus.zcodeHooksEnabled,
              "ZCode status separates registered hooks from the global switch");

        const auto minimaxInstall = manager.installMiniMaxCode();
        check(minimaxInstall.success && minimaxInstall.changed, "MiniMax Code installation");
        const auto pluginDirectory = (minimaxData / L"plugins") / L"ai-task-hub-bridge";
        check(std::filesystem::exists(pluginDirectory / L".minimax-plugin/plugin.json"), "MiniMax plugin manifest");
        check(std::filesystem::exists(pluginDirectory / L"hooks/hooks.json"), "MiniMax hooks installed");
        check(std::filesystem::exists(pluginDirectory / L"scripts/minimax_adapter.py"), "MiniMax adapter installed");
        check(std::filesystem::exists(pluginDirectory / L"icon.png"), "MiniMax icon installed");
        check(read(pluginDirectory / L".aihub-managed.json") ==
                  "{\"app\":\"ai-task-hub\",\"integration\":\"minimax-code\",\"managed\":true,\"version\":1}",
              "MiniMax ownership marker");
        const std::wstring minimaxScript = ((pluginDirectory / L"scripts") / L"minimax_adapter.py").wstring();
        const auto minimaxHooks = json(read(pluginDirectory / L"hooks/hooks.json"));
        const auto *minimaxCommand = minimaxHooks.get(L"hooks")->get(L"UserPromptSubmit")->array()[0]
                                         .get(L"hooks")->array()[0].get(L"command");
        check(minimaxCommand->string() == L"\"" + pythonPath + L"\" \"" + minimaxScript + L"\"",
              "MiniMax command placeholders substituted");
        check(!manager.installMiniMaxCode().changed, "MiniMax Code idempotence");
        check(manager.status().minimaxCodeInstalled, "MiniMax Code status detected");
        std::filesystem::remove(pluginDirectory / L".aihub-managed.json");
        const std::string foreignManifest = "{\"name\":\"foreign-plugin\"}";
        put(pluginDirectory / L".minimax-plugin/plugin.json", foreignManifest);
        check(!manager.installMiniMaxCode().success, "foreign plugin directory refused");
        check(read(pluginDirectory / L".minimax-plugin/plugin.json") == foreignManifest, "foreign plugin untouched");
        check(std::filesystem::exists(pluginDirectory / L"scripts/minimax_adapter.py"), "foreign plugin files kept");

        const std::string statusJson = manager.statusJson();
        check(statusJson.find("\"zcode\":") != std::string::npos, "status json reports zcode");
        check(statusJson.find("\"minimaxCode\":") != std::string::npos, "status json reports minimaxCode");
        check(statusJson.find("\"hooksEnabled\":") != std::string::npos, "status json reports zcode hooks state");
        check(statusJson.find("\"dataDir\":") != std::string::npos, "status json reports minimax data dir");

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
            const auto codexSession = store.ingest(json("{\"source\":\"CODEX\",\"externalTaskId\":\"codex-session\",\"eventType\":\"TASK_STARTED\",\"title\":\"排查 Docker 镜像拉取超时\"}"));
            check(codexSession > 0 && store.snapshot().counts.running == 1, "Codex prompt starts as running");
            check(store.ingest(json("{\"source\":\"CODEX\",\"externalTaskId\":\"codex-session\",\"eventType\":\"TASK_COMPLETED\",\"title\":\"Generate a concise, single-line task title\",\"contentPreview\":\"{\\\"title\\\":\\\"内部生成标题\\\"}\"}")) == codexSession,
                  "Codex completion reuses session task");
            auto codexTask = store.tasks(L"queue", L"", L"CODEX", L"", 20, 0);
            check(codexTask.size() == 1 && codexTask[0].title == L"排查 Docker 镜像拉取超时" &&
                  codexTask[0].status == L"COMPLETED_UNREAD", "completion preserves submitted prompt title");
            check(store.ingest(json("{\"source\":\"CODEX\",\"externalTaskId\":\"codex-session\",\"eventType\":\"TASK_STARTED\",\"title\":\"配置腾讯镜像源\"}")) == codexSession,
                  "next Codex prompt reuses session task");
            codexTask = store.tasks(L"queue", L"", L"CODEX", L"", 20, 0);
            check(codexTask.size() == 1 && codexTask[0].title == L"配置腾讯镜像源" &&
                  codexTask[0].status == L"RUNNING" && codexTask[0].completedAt.empty(),
                  "next prompt updates title and clears completed state");
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
        {
            // 新接入的平台必须被统一事件协议接受，否则事件会被 422 拒绝。
            TaskStore store((root / L"new-platform-database").wstring());
            check(store.ready(), "new platform database opens");
            const auto zcodeTask = store.ingest(json("{\"source\":\"ZCODE\",\"externalTaskId\":\"zcode-one\",\"eventType\":\"TASK_STARTED\",\"title\":\"ZCode 任务\"}"));
            const auto minimaxTask = store.ingest(json("{\"source\":\"MINIMAX_CODE\",\"externalTaskId\":\"minimax-one\",\"eventType\":\"TASK_COMPLETED\",\"title\":\"MiniMax Code 任务\"}"));
            check(zcodeTask > 0 && minimaxTask > 0, "ZCODE and MINIMAX_CODE events ingested");
            check(store.tasks(L"queue", L"", L"ZCODE", L"", 20, 0).size() == 1, "ZCODE source filtering");
            check(store.tasks(L"queue", L"", L"MINIMAX_CODE", L"", 20, 0).size() == 1, "MINIMAX_CODE source filtering");
            check(store.ingest(json("{\"source\":\"UNKNOWN_PLATFORM\",\"eventType\":\"TASK_STARTED\",\"title\":\"x\"}")) == 0,
                  "unknown source still rejected");
        }
        {
            // 更新检查：版本号提取与比较（不联网，纯逻辑）
            check(update_checker::extractVersionNumber(L"win32-v0.3.6") == L"0.3.6", "version from win32 tag");
            check(update_checker::extractVersionNumber(L"0.3.6-win32") == L"0.3.6", "version from app version");
            check(update_checker::extractVersionNumber(L"win32-v0.3.10") == L"0.3.10", "version keeps two digits");
            check(update_checker::compareVersions(L"0.3.6", L"0.3.7") < 0, "older version compares lower");
            check(update_checker::compareVersions(L"0.3.7", L"0.3.6") > 0, "newer version compares higher");
            check(update_checker::compareVersions(L"0.3.6", L"0.3.6") == 0, "same version compares equal");
            check(update_checker::compareVersions(L"0.3.10", L"0.3.9") > 0, "version compare is numeric not lexical");
            check(update_checker::compareVersions(L"0.4", L"0.3.9") > 0, "shorter version pads with zeros");
        }
        {
            // 更新检查：从 GitHub 发行列表里挑最新的 Win32 发行
            const std::string releases = R"json([
              {"tag_name":"v0.2.0","name":"Electron 版","html_url":"https://example.com/v020","prerelease":false,"draft":false},
              {"tag_name":"win32-v0.3.7","name":"预发布","html_url":"https://example.com/pre","prerelease":true,"draft":false},
              {"tag_name":"win32-v0.3.8","name":"草稿","html_url":"https://example.com/draft","prerelease":false,"draft":true},
              {"tag_name":"win32-v0.3.5","name":"AI Task Hub Win32 v0.3.5","html_url":"https://example.com/w035","prerelease":false,"draft":false,
               "published_at":"2026-09-01T00:00:00Z",
               "assets":[{"name":"AI-Task-Hub-Win32-x64-0.3.5.zip","browser_download_url":"https://example.com/old.zip"}]},
              {"tag_name":"win32-v0.3.6","name":"AI Task Hub Win32 v0.3.6","html_url":"https://example.com/w036","prerelease":false,"draft":false,
               "published_at":"2026-09-23T08:03:49Z",
               "assets":[{"name":"AI-Task-Hub-Win32-x64-0.3.6.sha256","browser_download_url":"https://example.com/hash.sha256"},
                         {"name":"AI-Task-Hub-Win32-x64-0.3.6.zip","browser_download_url":"https://example.com/new.zip"}]}
            ])json";
            update_checker::ReleaseInfo release;
            std::wstring error;
            check(update_checker::pickLatestWin32Release(releases, release, error), "win32 release picked");
            check(release.tag == L"win32-v0.3.6", "latest win32 release tag");
            check(release.name == L"AI Task Hub Win32 v0.3.6", "release name");
            check(release.htmlUrl == L"https://example.com/w036", "release page url");
            check(release.downloadUrl == L"https://example.com/new.zip", "portable zip url");
            check(update_checker::compareVersions(update_checker::extractVersionNumber(release.tag),
                                                  update_checker::extractVersionNumber(L"0.3.6-win32")) == 0,
                  "current build matches latest release");

            update_checker::ReleaseInfo missing;
            std::wstring missingError;
            check(!update_checker::pickLatestWin32Release("[]", missing, missingError), "empty release list fails");
            check(!missingError.empty(), "empty release list explains why");
            check(!update_checker::pickLatestWin32Release("not json", missing, missingError), "broken json fails");
        }
        std::cout << "PASS " << checks << " checks; isolated fixtures retained for inspection\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
