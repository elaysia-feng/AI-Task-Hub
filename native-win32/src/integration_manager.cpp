#include "integration_manager.h"

#include "json_lite.h"

#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace {

using Path = std::filesystem::path;

constexpr const wchar_t *kClaudeMarker = L"claude_adapter.py";
constexpr const wchar_t *kCodexMarker = L"notify_chain.py";
constexpr const wchar_t *kCodexPromptHookMarker = L"prompt_hook.py";
constexpr long long kHeartbeatTtlSeconds = 10 * 60;

bool fileExists(const std::wstring &path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool directoryExists(const std::wstring &path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::wstring trim(const std::wstring &value) {
    size_t first = 0;
    while (first < value.size() && iswspace(value[first])) ++first;
    size_t last = value.size();
    while (last > first && iswspace(value[last - 1])) --last;
    return value.substr(first, last - first);
}

bool readRaw(const std::wstring &path, std::string &result) {
    std::ifstream input(Path(path), std::ios::binary);
    if (!input) return false;
    result.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return true;
}

bool readUtf8(const std::wstring &path, std::string &result) {
    if (!readRaw(path, result)) return false;
    if (result.size() >= 3 && static_cast<unsigned char>(result[0]) == 0xef &&
        static_cast<unsigned char>(result[1]) == 0xbb && static_cast<unsigned char>(result[2]) == 0xbf) {
        result.erase(0, 3);
    }
    return true;
}

bool writeUtf8Atomic(const std::wstring &path, const std::string &content, std::wstring &error) {
    try {
        const Path target(path);
        std::filesystem::create_directories(target.parent_path());
        const std::wstring temp = path + L".aihub.tmp";
        std::ofstream output(Path(temp), std::ios::binary | std::ios::trunc);
        if (!output) {
            error = L"无法写入临时配置文件：" + temp;
            return false;
        }
        output.write(content.data(), static_cast<std::streamsize>(content.size()));
        output.flush();
        if (!output) {
            error = L"写入配置文件失败：" + path;
            return false;
        }
        output.close();
        if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(temp.c_str());
            error = L"替换配置文件失败（Windows 错误 " + std::to_wstring(GetLastError()) + L"）：" + path;
            return false;
        }
        return true;
    }
    catch (...) {
        error = L"创建配置目录失败：" + path;
        return false;
    }
}

bool backupConfig(const std::wstring &path, std::wstring &error) {
    if (!fileExists(path)) return true;
    // 每次改动前留存独立副本，不覆盖之前的恢复点。
    const std::wstring prefix = path + L".aihub-" + std::to_wstring(GetTickCount64());
    for (int index = 0; index < 100; ++index) {
        const std::wstring backup = prefix + (index == 0 ? L"" : L"-" + std::to_wstring(index)) + L".bak";
        if (CopyFileW(path.c_str(), backup.c_str(), TRUE)) return true;
        if (GetLastError() != ERROR_FILE_EXISTS) break;
    }
    error = L"创建配置备份失败，未修改原配置：" + path;
    return false;
}

std::wstring quoteWindowsArg(const std::wstring &value) {
    std::wstring result = L"\"";
    size_t backslashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++backslashes;
            result.push_back(ch);
            continue;
        }
        if (ch == L'\"') {
            result.insert(result.size() - backslashes, backslashes, L'\\');
            result += L"\\\"";
            backslashes = 0;
            continue;
        }
        backslashes = 0;
        result.push_back(ch);
    }
    // Windows 命令行规则要求结尾反斜杠在闭引号前再转义一次。
    if (backslashes > 0) result.insert(result.size(), backslashes, L'\\');
    result.push_back(L'\"');
    return result;
}

std::wstring tomlQuote(const std::wstring &value) {
    std::wstring result = L"\"";
    for (const wchar_t ch : value) {
        switch (ch) {
        case L'\\': result += L"\\\\"; break;
        case L'\"': result += L"\\\""; break;
        case L'\n': result += L"\\n"; break;
        case L'\r': result += L"\\r"; break;
        case L'\t': result += L"\\t"; break;
        default: result.push_back(ch); break;
        }
    }
    result.push_back(L'\"');
    return result;
}

std::wstring getEnvironment(const wchar_t *name) {
    const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
    if (size == 0) return {};
    std::wstring value(static_cast<size_t>(size), L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), size);
    if (length == 0 || length >= size) return {};
    value.resize(length);
    return value;
}

std::wstring resolveExecutable(const std::wstring &command) {
    if (command.empty()) return {};
    if (command.find_first_of(L"\"\r\n") != std::wstring::npos) return {};
    const Path candidate(command);
    if (candidate.has_parent_path()) return fileExists(command) ? command : L"";
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = SearchPathW(nullptr, command.c_str(), nullptr,
                                         static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (length == 0) return {};
        if (length < buffer.size()) return std::wstring(buffer.data(), length);
        buffer.resize(buffer.size() * 2);
    }
}

void replaceClaudeCommands(jsonlite::Value &value, const std::wstring &command, bool &replaced) {
    if (value.isArray()) {
        auto &items = std::get<jsonlite::Value::Array>(value.data);
        for (auto &item : items) replaceClaudeCommands(item, command, replaced);
        return;
    }
    if (value.isObject()) {
        auto &object = std::get<jsonlite::Value::Object>(value.data);
        for (auto &[key, item] : object) {
            if (key == L"command" && item.isString() && item.string().find(kClaudeMarker) != std::wstring::npos) {
                item = jsonlite::Value(command);
                replaced = true;
            } else if (key == L"hooks") replaceClaudeCommands(item, command, replaced);
        }
    }
}

jsonlite::Value makeClaudeEntry(const std::wstring &command) {
    jsonlite::Value::Object hook{
        {L"type", jsonlite::Value(L"command")},
        {L"command", jsonlite::Value(command)},
    };
    jsonlite::Value::Array hookList;
    hookList.emplace_back(std::move(hook));
    jsonlite::Value::Object entry{{L"hooks", jsonlite::Value(std::move(hookList))}};
    return jsonlite::Value(std::move(entry));
}

bool hasCodexPromptHook(const jsonlite::Value &value) {
    if (value.isArray()) {
        for (const auto &item : value.array())
            if (hasCodexPromptHook(item)) return true;
        return false;
    }
    if (!value.isObject()) return false;
    for (const auto &[key, item] : value.object()) {
        if ((key == L"command" || key == L"commandWindows" || key == L"command_windows") && item.isString() &&
            item.string().find(kCodexPromptHookMarker) != std::wstring::npos) return true;
        if (key == L"hooks" && hasCodexPromptHook(item)) return true;
    }
    return false;
}

bool hasCodexPromptHook(const jsonlite::Value &value, const std::wstring &expectedCommand) {
    if (value.isArray()) {
        for (const auto &item : value.array())
            if (hasCodexPromptHook(item, expectedCommand)) return true;
        return false;
    }
    if (!value.isObject()) return false;
    for (const auto &[key, item] : value.object()) {
        if ((key == L"command" || key == L"commandWindows" || key == L"command_windows") &&
            item.isString() && item.string().find(kCodexPromptHookMarker) != std::wstring::npos &&
            item.string() == expectedCommand) return true;
        if (key == L"hooks" && hasCodexPromptHook(item, expectedCommand)) return true;
    }
    return false;
}

void refreshCodexPromptHook(jsonlite::Value &value, const std::wstring &command,
                            bool &matched, bool &changed) {
    if (value.isArray()) {
        for (auto &item : std::get<jsonlite::Value::Array>(value.data))
            refreshCodexPromptHook(item, command, matched, changed);
        return;
    }
    if (!value.isObject()) return;
    for (auto &[key, item] : std::get<jsonlite::Value::Object>(value.data)) {
        if ((key == L"command" || key == L"commandWindows" || key == L"command_windows") &&
            item.isString() && item.string().find(kCodexPromptHookMarker) != std::wstring::npos) {
            matched = true;
            if (item.string() != command) {
                item = jsonlite::Value(command);
                changed = true;
            }
        } else if (key == L"hooks") {
            refreshCodexPromptHook(item, command, matched, changed);
        }
    }
}

jsonlite::Value makeCodexPromptHookEntry(const std::wstring &command) {
    jsonlite::Value::Object hook{
        {L"type", jsonlite::Value(L"command")},
        {L"command", jsonlite::Value(command)},
        {L"commandWindows", jsonlite::Value(command)},
        {L"timeout", jsonlite::Value(3.0)},
    };
    jsonlite::Value::Array hookList;
    hookList.emplace_back(std::move(hook));
    return jsonlite::Value(jsonlite::Value::Object{{L"hooks", jsonlite::Value(std::move(hookList))}});
}

bool ensureCodexPromptHook(jsonlite::Value::Object &root, const std::wstring &command,
                           std::wstring &error, bool &changed) {
    auto hooksIt = root.find(L"hooks");
    if (hooksIt == root.end()) hooksIt = root.emplace(L"hooks", jsonlite::Value(jsonlite::Value::Object{})).first;
    if (!hooksIt->second.isObject()) {
        error = L"Codex hooks.json 的 hooks 字段不是对象，未修改配置。";
        return false;
    }
    auto &hooks = std::get<jsonlite::Value::Object>(hooksIt->second.data);
    auto eventIt = hooks.find(L"UserPromptSubmit");
    if (eventIt == hooks.end()) eventIt = hooks.emplace(L"UserPromptSubmit", jsonlite::Value(jsonlite::Value::Array{})).first;
    if (!eventIt->second.isArray()) {
        error = L"Codex hooks.json 的 UserPromptSubmit 字段不是数组，未修改配置。";
        return false;
    }
    auto &entries = std::get<jsonlite::Value::Array>(eventIt->second.data);
    bool matched = false;
    for (auto &entry : entries) {
        refreshCodexPromptHook(entry, command, matched, changed);
    }
    if (matched) return true;
    entries.emplace_back(makeCodexPromptHookEntry(command));
    changed = true;
    return true;
}

void stripTomlMultilineStrings(std::wstring &line, bool &inside, wchar_t &quote) {
    const auto findClosingQuote = [](const std::wstring &text, size_t start, wchar_t quoteChar) {
        const std::wstring delimiter(3, quoteChar);
        size_t close = text.find(delimiter, start);
        while (quoteChar == L'"' && close != std::wstring::npos) {
            size_t backslashes = 0;
            for (size_t i = close; i > 0 && text[i - 1] == L'\\'; --i) ++backslashes;
            if (backslashes % 2 == 0) break;
            close = text.find(delimiter, close + 1);
        }
        return close;
    };
    if (inside) {
        const std::wstring delimiter(3, quote);
        const size_t close = findClosingQuote(line, 0, quote);
        if (close == std::wstring::npos) {
            line.clear();
            return;
        }
        line.erase(0, close + delimiter.size());
        inside = false;
        quote = 0;
    }

    size_t scan = 0;
    while (scan < line.size()) {
        size_t open = std::wstring::npos;
        wchar_t openQuote = 0;
        bool inBasicString = false;
        bool inLiteralString = false;
        bool escaped = false;
        for (size_t i = scan; i < line.size(); ++i) {
            const wchar_t ch = line[i];
            if (inBasicString && escaped) { escaped = false; continue; }
            if (inBasicString && ch == L'\\') { escaped = true; continue; }
            if (inBasicString && ch == L'"') { inBasicString = false; continue; }
            if (inLiteralString && ch == L'\'') { inLiteralString = false; continue; }
            if (inBasicString || inLiteralString) continue;
            if (ch == L'#') break;
            if (ch == L'"' || ch == L'\'') {
                if (i + 2 < line.size() && line[i + 1] == ch && line[i + 2] == ch) {
                    open = i;
                    openQuote = ch;
                    break;
                }
                if (ch == L'"') inBasicString = true;
                else inLiteralString = true;
            }
        }
        if (open == std::wstring::npos) return;

        const std::wstring delimiter(3, openQuote);
        const size_t close = findClosingQuote(line, open + delimiter.size(), openQuote);
        const std::wstring prefix = line.substr(0, open);
        if (close == std::wstring::npos) {
            line = prefix;
            inside = true;
            quote = openQuote;
            return;
        }
        line = prefix + line.substr(close + delimiter.size());
        scan = prefix.size();
    }
}

bool parseTomlBoolean(const std::wstring &text, const std::wstring &section,
                      const std::wstring &key, bool &found, bool &value) {
    found = false;
    std::wstring currentSection;
    const auto parseValue = [&](const std::wstring &raw) {
        const std::wstring booleanText = trim(raw);
        if (booleanText != L"true" && booleanText != L"false") return false;
        found = true;
        value = booleanText == L"true";
        return true;
    };
    const auto inlineField = [&](const std::wstring &raw, std::wstring &fieldValue) {
        if (raw.size() < 2 || raw.front() != L'{' || raw.back() != L'}') return false;
        const std::wstring body = raw.substr(1, raw.size() - 2);
        size_t entryStart = 0;
        int nestedTables = 0;
        int nestedArrays = 0;
        wchar_t quote = 0;
        bool escaped = false;
        for (size_t i = 0; i <= body.size(); ++i) {
            const wchar_t ch = i == body.size() ? L',' : body[i];
            if (quote == L'"' && escaped) { escaped = false; continue; }
            if (quote == L'"' && ch == L'\\') { escaped = true; continue; }
            if (quote && ch == quote) { quote = 0; continue; }
            if (!quote && (ch == L'"' || ch == L'\'')) { quote = ch; continue; }
            if (quote) continue;
            if (ch == L'{') ++nestedTables;
            else if (ch == L'}') --nestedTables;
            else if (ch == L'[') ++nestedArrays;
            else if (ch == L']') --nestedArrays;
            else if (ch == L',' && nestedTables == 0 && nestedArrays == 0) {
                const std::wstring entry = trim(body.substr(entryStart, i - entryStart));
                const size_t equals = entry.find(L'=');
                if (equals != std::wstring::npos) {
                    std::wstring entryKey = trim(entry.substr(0, equals));
                    if (entryKey.size() >= 2 &&
                        ((entryKey.front() == L'"' && entryKey.back() == L'"') ||
                         (entryKey.front() == L'\'' && entryKey.back() == L'\''))) {
                        entryKey = entryKey.substr(1, entryKey.size() - 2);
                    }
                    if (entryKey == key) {
                        fieldValue = trim(entry.substr(equals + 1));
                        return true;
                    }
                }
                entryStart = i + 1;
            }
        }
        return false;
    };
    bool insideMultilineString = false;
    wchar_t multilineQuote = 0;
    size_t lineStart = 0;
    while (lineStart <= text.size()) {
        const size_t lineEnd = text.find(L'\n', lineStart);
        std::wstring line = text.substr(lineStart, lineEnd == std::wstring::npos ? std::wstring::npos : lineEnd - lineStart);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        stripTomlMultilineStrings(line, insideMultilineString, multilineQuote);
        bool inString = false;
        bool escaped = false;
        for (size_t i = 0; i < line.size(); ++i) {
            if (inString && escaped) { escaped = false; continue; }
            if (inString && line[i] == L'\\') { escaped = true; continue; }
            if (line[i] == L'"') { inString = !inString; continue; }
            if (!inString && line[i] == L'#') { line.resize(i); break; }
        }
        line = trim(line);
        if (!line.empty() && line.front() == L'[' && line.back() == L']') {
            currentSection = trim(line.substr(1, line.size() - 2));
        } else if (currentSection == section || (currentSection.empty() && !section.empty())) {
            const size_t equals = line.find(L'=');
            if (equals != std::wstring::npos) {
                const std::wstring field = trim(line.substr(0, equals));
                const std::wstring raw = trim(line.substr(equals + 1));
                if (currentSection == section && field == key) {
                    return parseValue(raw);
                }
                if (currentSection.empty() && !section.empty()) {
                    if (field == section + L"." + key) return parseValue(raw);
                    if (field == section) {
                        std::wstring nestedValue;
                        if (inlineField(raw, nestedValue)) return parseValue(nestedValue);
                    }
                }
            }
        }
        if (lineEnd == std::wstring::npos) break;
        lineStart = lineEnd + 1;
    }
    return true;
}

bool codexPromptHookPolicyError(const std::wstring &userConfig, std::wstring &error) {
    bool userHooksFound = false;
    bool userHooks = true;
    bool userCodexHooksFound = false;
    bool userCodexHooks = true;
    if (!parseTomlBoolean(userConfig, L"features", L"hooks", userHooksFound, userHooks) ||
        !parseTomlBoolean(userConfig, L"features", L"codex_hooks", userCodexHooksFound, userCodexHooks)) {
        error = L"无法解析 Codex 的 features.hooks/codex_hooks 设置，不能确认提问 hook 是否允许。";
        return true;
    }
    if (!userHooksFound && userCodexHooksFound) {
        userHooksFound = true;
        userHooks = userCodexHooks;
    }

    bool managedHooksFound = false;
    bool managedHooks = false;
    bool managedCodexHooksFound = false;
    bool managedCodexHooks = false;
    bool managedOnlyFound = false;
    bool managedOnly = false;
    std::wstring programData = getEnvironment(L"PROGRAMDATA");
    if (programData.empty()) programData = L"C:\\ProgramData";
    if (!programData.empty()) {
        const std::wstring requirementsPath = (Path(programData) / L"OpenAI" / L"Codex" / L"requirements.toml").wstring();
        if (fileExists(requirementsPath)) {
            std::string requirementsUtf8;
            if (!readUtf8(requirementsPath, requirementsUtf8)) {
                error = L"无法读取 Codex 组织策略，不能确认提问 hook 是否允许。";
                return true;
            }
            const std::wstring requirements = jsonlite::fromUtf8(requirementsUtf8);
            if (!parseTomlBoolean(requirements, L"features", L"hooks", managedHooksFound, managedHooks) ||
                !parseTomlBoolean(requirements, L"features", L"codex_hooks", managedCodexHooksFound, managedCodexHooks) ||
                !parseTomlBoolean(requirements, L"", L"allow_managed_hooks_only", managedOnlyFound, managedOnly)) {
                error = L"无法解析 Codex 组织策略，不能确认提问 hook 是否允许。";
                return true;
            }
            if (!managedHooksFound && managedCodexHooksFound) {
                managedHooksFound = true;
                managedHooks = managedCodexHooks;
            }
        }
    }

    if (managedHooksFound && !managedHooks) {
        error = L"Codex 组织策略已禁用 hooks，提问状态 hook 不会运行。";
        return true;
    }
    if (managedOnlyFound && managedOnly) {
        error = L"Codex 组织策略仅允许托管 hooks，AI Task Hub 的用户 hook 不会运行。";
        return true;
    }
    if (userHooksFound && !userHooks && !(managedHooksFound && managedHooks)) {
        error = L"Codex config.toml 中 features.hooks/codex_hooks=false，提问状态 hook 不会运行。";
        return true;
    }
    return false;
}

std::wstring normalizedPath(const std::wstring &path) {
    std::wstring result = Path(path).lexically_normal().wstring();
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t ch) { return std::towlower(ch); });
    return result;
}

bool parseTomlString(const std::wstring &value, size_t &position, std::wstring &result) {
    if (position >= value.size()) return false;
    const wchar_t quote = value[position];
    if (quote != L'"' && quote != L'\'') return false;
    ++position;
    result.clear();
    while (position < value.size()) {
        const wchar_t ch = value[position++];
        if (ch == quote) return true;
        if (quote == L'\'' || ch != L'\\') {
            result.push_back(ch);
            continue;
        }
        if (position >= value.size()) return false;
        const wchar_t escaped = value[position++];
        switch (escaped) {
        case L'\\': result.push_back(L'\\'); break;
        case L'\"': result.push_back(L'\"'); break;
        case L'n': result.push_back(L'\n'); break;
        case L'r': result.push_back(L'\r'); break;
        case L't': result.push_back(L'\t'); break;
        case L'b': result.push_back(L'\b'); break;
        case L'f': result.push_back(L'\f'); break;
        default: return false;
        }
    }
    return false;
}

bool parseTomlNotify(const std::wstring &value, std::vector<std::wstring> &commands) {
    commands.clear();
    size_t position = 0;
    const auto skip = [&]() {
        for (;;) {
            while (position < value.size() && iswspace(value[position])) ++position;
            if (position >= value.size() || value[position] != L'#') break;
            while (position < value.size() && value[position] != L'\n') ++position;
        }
    };
    skip();
    if (position >= value.size()) return false;
    if (value[position] == L'"' || value[position] == L'\'') {
        std::wstring command;
        if (!parseTomlString(value, position, command)) return false;
        commands.push_back(std::move(command));
        skip();
        return position == value.size();
    }
    if (value[position] != L'[') return false;
    ++position;
    for (;;) {
        skip();
        if (position < value.size() && value[position] == L']') { ++position; skip(); return position == value.size(); }
        if (position >= value.size()) return false;
        std::wstring command;
        if (!parseTomlString(value, position, command)) return false;
        commands.push_back(std::move(command));
        skip();
        if (position < value.size() && value[position] == L']') { ++position; skip(); return position == value.size(); }
        if (position >= value.size() || value[position] != L',') return false;
        ++position;
    }
}

size_t tomlValueEnd(const std::wstring &text, size_t valueStart, size_t lineEnd) {
    size_t position = valueStart;
    while (position < lineEnd && iswspace(text[position])) ++position;
    if (position >= lineEnd) return lineEnd;
    const bool array = text[position] == L'[';
    int depth = 0;
    wchar_t quote = 0;
    bool escaped = false;
    for (size_t i = position; i < text.size(); ++i) {
        const wchar_t ch = text[i];
        if (quote != 0) {
            if (quote == L'"' && escaped) { escaped = false; continue; }
            if (quote == L'"' && ch == L'\\') { escaped = true; continue; }
            if (ch == quote) quote = 0;
            continue;
        }
        if (ch == L'#') {
            if (!array) return i;
            const size_t next = text.find(L'\n', i);
            if (next == std::wstring::npos) return text.size();
            i = next;
            continue;
        }
        if (ch == L'"' || ch == L'\'') { quote = ch; continue; }
        if (ch == L'[') ++depth;
        else if (ch == L']' && --depth == 0) return i + 1;
        if ((ch == L'\n' || ch == L'\r') && depth == 0) return i;
    }
    return text.size();
}

struct NotifyLocation {
    bool found = false;
    bool supported = true;
    size_t start = 0;
    size_t end = 0;
    std::wstring value;
};

NotifyLocation findTomlNotify(const std::wstring &text) {
    NotifyLocation location;
    size_t lineStart = 0;
    while (lineStart < text.size()) {
        const size_t lineEnd = text.find(L'\n', lineStart);
        const size_t rawLineEnd = lineEnd == std::wstring::npos ? text.size() : lineEnd;
        size_t contentEnd = rawLineEnd;
        if (contentEnd > lineStart && text[contentEnd - 1] == L'\r') --contentEnd;
        const size_t first = text.find_first_not_of(L" \t\r", lineStart);
        if (first < contentEnd && text[first] != L'#') {
            // notify 属于 TOML 根表；绝不能改动某个 provider/profile 内的同名键。
            if (text[first] == L'[') break;
            const size_t equals = text.find(L'=', first);
            if (equals != std::wstring::npos && equals < contentEnd) {
                const std::wstring key = trim(text.substr(first, equals - first));
                const size_t valueStart = text.find_first_not_of(L" \t", equals + 1);
                if (valueStart == std::wstring::npos || valueStart >= contentEnd ||
                    text.compare(valueStart, 3, L"\"\"\"") == 0 || text.compare(valueStart, 3, L"'''") == 0) {
                    // 不猜测复杂/残缺 TOML 的边界，宁可保留配置并提示手工处理。
                    location.supported = false;
                    return location;
                }
                const size_t valueEnd = tomlValueEnd(text, valueStart, contentEnd);
                if (key == L"notify" || key == L"\"notify\"" || key == L"'notify'") {
                    if (location.found) { location.supported = false; return location; }
                    location.found = true;
                    location.start = valueStart;
                    location.end = valueEnd;
                    location.value = text.substr(valueStart, valueEnd - valueStart);
                }
                const size_t nextLine = text.find(L'\n', valueEnd);
                if (nextLine == std::wstring::npos) break;
                lineStart = nextLine + 1;
                continue;
            }
        }
        if (lineEnd == std::wstring::npos) break;
        lineStart = lineEnd + 1;
    }
    return location;
}

long long unixSeconds() {
    return static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
}

} // namespace

IntegrationManager::IntegrationManager(std::wstring profile, std::wstring data, std::wstring adapters)
    : executableDirectory_(executableDirectory()), profileOverride_(std::move(profile)),
      dataOverride_(std::move(data)), adaptersOverride_(std::move(adapters)) {}

std::wstring IntegrationManager::executableDirectory() const {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1) return Path(std::wstring(buffer.data(), length)).parent_path().wstring();
        buffer.resize(buffer.size() * 2);
    }
}

std::wstring IntegrationManager::userProfileDirectory() const {
    if (!profileOverride_.empty()) return profileOverride_;
    PWSTR raw = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, &raw)) && raw) {
        const std::wstring result(raw);
        CoTaskMemFree(raw);
        return result;
    }
    return getEnvironment(L"USERPROFILE");
}

std::wstring IntegrationManager::userDataDirectory() const {
    if (!dataOverride_.empty()) return dataOverride_;
    std::wstring base = getEnvironment(L"APPDATA");
    if (base.empty()) base = userProfileDirectory();
    return (Path(base) / L"AI Task Hub").wstring();
}

std::wstring IntegrationManager::resourceAdaptersDirectory() const {
    if (!adaptersOverride_.empty()) return adaptersOverride_;
    const std::vector<Path> candidates{
        Path(executableDirectory_) / L"adapters",
        Path(executableDirectory_).parent_path() / L"adapters",
        Path(executableDirectory_).parent_path().parent_path() / L"adapters",
        std::filesystem::current_path() / L"adapters",
    };
    for (const auto &candidate : candidates) {
        if (directoryExists(candidate.wstring())) return candidate.wstring();
    }
    return (Path(executableDirectory_) / L"adapters").wstring();
}

std::wstring IntegrationManager::claudeAdapterDirectory() const {
    return (Path(userDataDirectory()) / L"adapters" / L"claude-code").wstring();
}

std::wstring IntegrationManager::codexAdapterDirectory() const {
    return (Path(userDataDirectory()) / L"adapters" / L"codex").wstring();
}

std::wstring IntegrationManager::chatgptExtensionDirectory() const {
    return (Path(userDataDirectory()) / L"chatgpt-extension").wstring();
}

std::wstring IntegrationManager::heartbeatPath() const {
    return (Path(userDataDirectory()) / L"chatgpt_heartbeat.json").wstring();
}

std::wstring IntegrationManager::findPython() const {
    const std::wstring configured = getEnvironment(L"AIHUB_PYTHON");
    if (!configured.empty()) return resolveExecutable(configured);
    for (const wchar_t *name : {L"python.exe", L"python3.exe", L"py.exe"}) {
        const std::wstring resolved = resolveExecutable(name);
        if (!resolved.empty()) return resolved;
    }
    return {};
}

bool IntegrationManager::materializeDirectory(const std::wstring &source, const std::wstring &target,
                                              const std::wstring &requiredFiles, std::wstring &error) const {
    try {
        if (!directoryExists(source)) {
            error = L"应用资源缺失：" + source;
            return false;
        }
        std::filesystem::create_directories(Path(target));
        size_t start = 0;
        while (start <= requiredFiles.size()) {
            const size_t separator = requiredFiles.find(L';', start);
            const std::wstring name = requiredFiles.substr(start, separator == std::wstring::npos ? std::wstring::npos : separator - start);
            if (!name.empty()) {
                const std::wstring from = (Path(source) / name).wstring();
                const std::wstring to = (Path(target) / name).wstring();
                if (!fileExists(from)) {
                    error = L"应用资源缺失：" + from;
                    return false;
                }
                bool alreadyCurrent = false;
                if (fileExists(to)) {
                    std::ifstream sourceFile(Path(from), std::ios::binary);
                    std::ifstream targetFile(Path(to), std::ios::binary);
                    alreadyCurrent = sourceFile && targetFile &&
                                     std::equal(std::istreambuf_iterator<char>(sourceFile), std::istreambuf_iterator<char>(),
                                                std::istreambuf_iterator<char>(targetFile), std::istreambuf_iterator<char>());
                }
                if (!alreadyCurrent) {
                    const DWORD targetAttributes = GetFileAttributesW(to.c_str());
                    const bool targetReadOnly = targetAttributes != INVALID_FILE_ATTRIBUTES &&
                                                (targetAttributes & FILE_ATTRIBUTE_READONLY) != 0;
                    if (targetReadOnly && !SetFileAttributesW(to.c_str(), targetAttributes & ~FILE_ATTRIBUTE_READONLY)) {
                        const DWORD attributeError = GetLastError();
                        error = L"无法更新只读适配器文件（Windows 错误 " + std::to_wstring(attributeError) + L"）：" + from + L" -> " + to;
                        return false;
                    }
                    if (!CopyFileW(from.c_str(), to.c_str(), FALSE)) {
                        const DWORD copyError = GetLastError();
                        if (targetReadOnly) SetFileAttributesW(to.c_str(), targetAttributes);
                        error = L"复制适配器文件失败（Windows 错误 " + std::to_wstring(copyError) + L"）：" + from + L" -> " + to;
                        return false;
                    }
                }
            }
            if (separator == std::wstring::npos) break;
            start = separator + 1;
        }
        return true;
    }
    catch (...) {
        error = L"创建适配器目录失败：" + target;
        return false;
    }
}

IntegrationStatus IntegrationManager::status() const {
    IntegrationStatus result;
    const std::wstring profile = userProfileDirectory();
    result.claudeSettingsPath = (Path(profile) / L".claude" / L"settings.json").wstring();
    result.codexConfigPath = (Path(profile) / L".codex" / L"config.toml").wstring();
    result.codexHooksPath = (Path(profile) / L".codex" / L"hooks.json").wstring();
    result.chatgptExtensionDirectory = chatgptExtensionDirectory();
    result.pythonCommand = findPython();

    std::string text;
    if (readUtf8(result.claudeSettingsPath, text)) {
        jsonlite::Value config;
        std::string error;
        if (jsonlite::parseUtf8(text, config, error)) {
            const auto *hooks = config.get(L"hooks");
            bool complete = hooks && hooks->isObject();
            for (const auto *event : {L"UserPromptSubmit", L"Notification", L"Stop"}) {
                const auto *entries = hooks ? hooks->get(event) : nullptr;
                bool found = false;
                if (entries && entries->isArray()) {
                    auto copy = *entries;
                    replaceClaudeCommands(copy, L"", found);
                }
                complete = complete && found;
            }
            result.claudeInstalled = complete;
        }
    }
    std::wstring codexConfigText;
    if (readUtf8(result.codexConfigPath, text)) {
        codexConfigText = jsonlite::fromUtf8(text);
        const auto location = findTomlNotify(jsonlite::fromUtf8(text));
        std::vector<std::wstring> command;
        if (location.supported && location.found && parseTomlNotify(location.value, command)) {
            const std::wstring currentChain = (Path(codexAdapterDirectory()) / L"notify_chain.py").wstring();
            result.codexNotifyInstalled = command.size() == 2 &&
                normalizedPath(command[0]) == normalizedPath(result.pythonCommand) &&
                command[1].find(kCodexMarker) != std::wstring::npos &&
                normalizedPath(command[1]) == normalizedPath(currentChain) && fileExists(command[1]);
        }
    }
    const std::wstring promptHookPath = (Path(codexAdapterDirectory()) / L"prompt_hook.py").wstring();
    std::wstring policyError;
    const bool policyBlocked = codexPromptHookPolicyError(codexConfigText, policyError);
    if (policyBlocked) result.codexPromptHookBlockReason = policyError;
    if (readUtf8(result.codexHooksPath, text)) {
        jsonlite::Value hooksConfig;
        std::string parseError;
        if (jsonlite::parseUtf8(text, hooksConfig, parseError) && hooksConfig.isObject()) {
            const auto *hooks = hooksConfig.get(L"hooks");
            const auto *entries = hooks && hooks->isObject() ? hooks->get(L"UserPromptSubmit") : nullptr;
            const std::wstring expectedCommand = quoteWindowsArg(result.pythonCommand) + L" " +
                                                 quoteWindowsArg(promptHookPath);
            result.codexPromptHookInstalled = !policyBlocked && entries && entries->isArray() &&
                hasCodexPromptHook(*entries, expectedCommand) && fileExists(promptHookPath);
            if (!policyBlocked && entries && entries->isArray() && hasCodexPromptHook(*entries) &&
                !result.codexPromptHookInstalled) {
                result.codexPromptHookBlockReason = L"hooks.json 中仍指向旧版提问 hook 路径，请重新接入以更新路径。";
            }
        }
    }
    result.codexInstalled = result.codexNotifyInstalled && result.codexPromptHookInstalled;
    result.chatgptPrepared = fileExists((Path(result.chatgptExtensionDirectory) / L"manifest.json").wstring());

    if (readUtf8(heartbeatPath(), text)) {
        jsonlite::Value heartbeat;
        std::string parseError;
        if (jsonlite::parseUtf8(text, heartbeat, parseError) && heartbeat.isObject()) {
            const jsonlite::Value *timestamp = heartbeat.get(L"ts");
            result.chatgptOnline = timestamp && timestamp->isNumber() &&
                                   timestamp->number() <= static_cast<double>(unixSeconds()) &&
                                   unixSeconds() - static_cast<long long>(timestamp->number()) < kHeartbeatTtlSeconds;
        }
    }
    return result;
}

IntegrationResult IntegrationManager::installClaude() {
    std::lock_guard<std::mutex> lock(writeMutex_);
    IntegrationResult result;
    const std::wstring python = findPython();
    if (python.empty()) {
        result.message = L"未检测到 Python。请安装 Python，或设置 AIHUB_PYTHON 后重试。";
        return result;
    }
    std::wstring error;
    const std::wstring source = (Path(resourceAdaptersDirectory()) / L"claude-code").wstring();
    const std::wstring target = claudeAdapterDirectory();
    if (!materializeDirectory(source, target, L"claude_adapter.py", error)) {
        result.message = error;
        return result;
    }
    const std::wstring settingsPath = (Path(userProfileDirectory()) / L".claude" / L"settings.json").wstring();
    const std::wstring command = quoteWindowsArg(python) + L" " +
                                 quoteWindowsArg((Path(target) / L"claude_adapter.py").wstring());
    std::string utf8;
    jsonlite::Value root(jsonlite::Value::Object{});
    if (fileExists(settingsPath)) {
        if (!readUtf8(settingsPath, utf8)) {
            result.message = L"无法读取 Claude Code 配置：" + settingsPath;
            return result;
        }
        std::string parseError;
        if (!jsonlite::parseUtf8(utf8, root, parseError) || !root.isObject()) {
            result.message = L"Claude Code 的 settings.json 解析失败，请先手工修复 JSON。";
            return result;
        }
    }

    const std::string original = jsonlite::stringifyUtf8(root);
    auto &rootObject = std::get<jsonlite::Value::Object>(root.data);
    auto hooksIt = rootObject.find(L"hooks");
    if (hooksIt == rootObject.end()) hooksIt = rootObject.emplace(L"hooks", jsonlite::Value(jsonlite::Value::Object{})).first;
    if (!hooksIt->second.isObject()) {
        result.message = L"Claude Code 的 hooks 字段不是对象，未修改配置。";
        return result;
    }
    auto &hooks = std::get<jsonlite::Value::Object>(hooksIt->second.data);
    constexpr const wchar_t *events[] = {L"UserPromptSubmit", L"Notification", L"Stop"};
    for (const wchar_t *event : events) {
        auto eventIt = hooks.find(event);
        if (eventIt == hooks.end()) eventIt = hooks.emplace(event, jsonlite::Value(jsonlite::Value::Array{})).first;
        if (!eventIt->second.isArray()) {
            result.message = L"Claude Code 的 hooks." + std::wstring(event) + L" 不是数组，未修改配置。";
            return result;
        }
        auto &entries = std::get<jsonlite::Value::Array>(eventIt->second.data);
        bool eventHasAdapter = false;
        for (auto &entry : entries) {
            bool replaced = false;
            replaceClaudeCommands(entry, command, replaced);
            if (replaced) eventHasAdapter = true;
        }
        if (!eventHasAdapter) {
            entries.push_back(makeClaudeEntry(command));
        }
    }
    if (original == jsonlite::stringifyUtf8(root)) {
        result.success = true;
        result.message = L"Claude Code 已接入，无需重复修改。";
        return result;
    }
    if (!backupConfig(settingsPath, error) || !writeUtf8Atomic(settingsPath, jsonlite::stringifyUtf8(root) + "\n", error)) {
        result.message = error;
        return result;
    }
    result.success = true;
    result.changed = true;
    result.message = L"Claude Code 已接入：" + settingsPath;
    return result;
}

IntegrationResult IntegrationManager::installCodex() {
    std::lock_guard<std::mutex> lock(writeMutex_);
    IntegrationResult result;
    const std::wstring python = findPython();
    if (python.empty()) {
        result.message = L"未检测到 Python。请安装 Python，或设置 AIHUB_PYTHON 后重试。";
        return result;
    }
    std::wstring error;
    const std::wstring source = (Path(resourceAdaptersDirectory()) / L"codex").wstring();
    const std::wstring target = codexAdapterDirectory();
    if (!materializeDirectory(source, target, L"notify_chain.py;event_converter.py;prompt_hook.py", error)) {
        result.message = error;
        return result;
    }
    const std::wstring configPath = (Path(userProfileDirectory()) / L".codex" / L"config.toml").wstring();
    const std::wstring hooksPath = (Path(userProfileDirectory()) / L".codex" / L"hooks.json").wstring();
    std::string utf8;
    const bool configExisted = fileExists(configPath);
    std::string originalConfigRaw;
    if (configExisted && !readRaw(configPath, originalConfigRaw)) {
        result.message = L"无法备份 Codex 配置，未修改配置：" + configPath;
        return result;
    }
    if (!readUtf8(configPath, utf8) && fileExists(configPath)) {
        result.message = L"无法读取 Codex 配置：" + configPath;
        return result;
    }
    std::wstring text = jsonlite::fromUtf8(utf8);
    if (!utf8.empty() && text.empty()) {
        result.message = L"Codex 的 config.toml 不是有效 UTF-8，未修改配置。";
        return result;
    }
    std::wstring policyError;
    if (codexPromptHookPolicyError(text, policyError)) {
        result.message = policyError;
        return result;
    }
    const std::wstring chain = (Path(target) / L"notify_chain.py").wstring();
    const std::wstring promptHook = (Path(target) / L"prompt_hook.py").wstring();
    const std::wstring replacement = L"[" + tomlQuote(python) + L", " + tomlQuote(chain) + L"]";
    const NotifyLocation location = findTomlNotify(text);
    std::vector<std::wstring> existing;
    if (!location.supported || (location.found && !parseTomlNotify(location.value, existing))) {
        result.message = L"Codex 的 notify 配置格式暂不支持，未修改 config.toml。";
        return result;
    }
    jsonlite::Value hooksConfig(jsonlite::Value::Object{});
    std::string hooksText;
    if (fileExists(hooksPath)) {
        if (!readUtf8(hooksPath, hooksText)) {
            result.message = L"无法读取 Codex hooks.json，未修改配置：" + hooksPath;
            return result;
        }
        std::string parseError;
        if (!jsonlite::parseUtf8(hooksText, hooksConfig, parseError) || !hooksConfig.isObject()) {
            result.message = L"Codex 的 hooks.json 解析失败，请先手工修复 JSON。";
            return result;
        }
    }
    auto &hooksObject = std::get<jsonlite::Value::Object>(hooksConfig.data);
    bool hooksChanged = false;
    const std::wstring promptCommand = quoteWindowsArg(python) + L" " + quoteWindowsArg(promptHook);
    if (!ensureCodexPromptHook(hooksObject, promptCommand, error, hooksChanged)) {
        result.message = error;
        return result;
    }
    bool hasChain = false;
    for (const auto &command : existing) if (command.find(kCodexMarker) != std::wstring::npos) hasChain = true;
    const std::wstring forwardPath = (Path(target) / L"forward_target.json").wstring();
    const bool forwardExisted = fileExists(forwardPath);
    bool forwardCreated = false;
    if (!existing.empty() && !hasChain && !forwardExisted) {
        jsonlite::Value::Array commandArray;
        for (const auto &command : existing) commandArray.emplace_back(command);
        jsonlite::Value::Object forward{{L"command", jsonlite::Value(std::move(commandArray))}};
        if (!backupConfig(forwardPath, error) || !writeUtf8Atomic(forwardPath, jsonlite::stringifyUtf8(jsonlite::Value(std::move(forward))) + "\n", error)) {
            result.message = error;
            return result;
        }
        forwardCreated = true;
    }
    if (hasChain && existing.size() >= 2) {
        // 旧版桌面端可能已经接入，但链脚本位于旧目录；尽量把旧链的转发备份迁移过来。
        const auto oldCommand = std::find_if(existing.begin(), existing.end(), [](const std::wstring &command) {
            return command.find(kCodexMarker) != std::wstring::npos;
        });
        const Path oldForward = oldCommand == existing.end()
            ? Path{}
            : Path(*oldCommand).parent_path() / L"forward_target.json";
        if (!fileExists(forwardPath) && !oldForward.empty() && fileExists(oldForward.wstring())) {
            if (!CopyFileW(oldForward.wstring().c_str(), forwardPath.c_str(), TRUE)) {
                result.message = L"迁移旧通知转发配置失败，未修改 Codex 配置。";
                return result;
            }
            forwardCreated = true;
        }
    }
    result.forwardTarget = fileExists(forwardPath);
    const bool changed = !location.found ||
                         trim(location.value) != replacement;
    bool configWritten = false;
    const auto rollbackConfig = [&]() {
        if (configWritten) {
            std::wstring rollbackError;
            bool restored = false;
            if (configExisted) {
                restored = writeUtf8Atomic(configPath, originalConfigRaw, rollbackError);
            } else if (DeleteFileW(configPath.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) {
                restored = true;
            } else {
                rollbackError = L"删除新建的 config.toml 失败（Windows 错误 " + std::to_wstring(GetLastError()) + L"）。";
            }
            if (!restored) error += L"；回滚 config.toml 失败：" + rollbackError;
        }
        if (forwardCreated) DeleteFileW(forwardPath.c_str());
    };
    if (changed) {
        if (location.found) text.replace(location.start, location.end - location.start, replacement + L" ");
        else {
            text = L"notify = " + replacement + L"\n" + text;
        }
        if (!backupConfig(configPath, error) || !writeUtf8Atomic(configPath, jsonlite::toUtf8(text), error)) {
            if (forwardCreated) DeleteFileW(forwardPath.c_str());
            result.message = error;
            return result;
        }
        configWritten = true;
    }
    if (hooksChanged) {
        if (!backupConfig(hooksPath, error) ||
            !writeUtf8Atomic(hooksPath, jsonlite::stringifyUtf8(hooksConfig) + "\n", error)) {
            rollbackConfig();
            result.message = error;
            return result;
        }
    }
    result.success = true;
    result.changed = changed || hooksChanged || (!existing.empty() && !hasChain && fileExists(forwardPath));
    result.message = result.changed
        ? L"Codex 状态接入已更新。首次使用请执行 /hooks 并信任 UserPromptSubmit 钩子，然后重启 Codex。"
        : L"Codex 已接入。首次使用请执行 /hooks 并信任 UserPromptSubmit 钩子。";
    return result;
}

IntegrationResult IntegrationManager::prepareChatGptExtension() {
    std::lock_guard<std::mutex> lock(writeMutex_);
    IntegrationResult result;
    std::wstring error;
    const std::wstring source = (Path(resourceAdaptersDirectory()) / L"chatgpt-extension").wstring();
    const std::wstring target = chatgptExtensionDirectory();
    if (!materializeDirectory(source, target, L"background.js;content.js;manifest.json;README.md", error)) {
        result.message = error;
        return result;
    }
    result.success = true;
    result.changed = true;
    result.message = L"ChatGPT 扩展目录已准备：" + target + L"\n请在 chrome://extensions 中开启开发者模式并加载此目录。";
    return result;
}

bool IntegrationManager::recordChatGptHeartbeat(const std::string &body) {
    std::lock_guard<std::mutex> lock(writeMutex_);
    jsonlite::Value request;
    std::string parseError;
    if (!jsonlite::parseUtf8(body, request, parseError) || !request.isObject()) return false;
    const jsonlite::Value *version = request.get(L"version");
    const std::wstring versionText = version && version->isString() ? version->string() : L"";
    jsonlite::Value::Object heartbeat{
        {L"ts", jsonlite::Value(static_cast<double>(unixSeconds()))},
        {L"version", jsonlite::Value(versionText)},
    };
    std::wstring error;
    return writeUtf8Atomic(heartbeatPath(), jsonlite::stringifyUtf8(jsonlite::Value(std::move(heartbeat))) + "\n", error);
}

std::string IntegrationManager::statusJson() const {
    const IntegrationStatus info = status();
    return std::string("{\"claudeCode\":{\"installed\":") + (info.claudeInstalled ? "true" : "false") +
           ",\"settingsPath\":\"" + jsonlite::escapeUtf8(info.claudeSettingsPath) +
           "\"},\"codex\":{\"installed\":" + (info.codexInstalled ? "true" : "false") +
           ",\"notifyInstalled\":" + (info.codexNotifyInstalled ? "true" : "false") +
           ",\"promptHookInstalled\":" + (info.codexPromptHookInstalled ? "true" : "false") +
           ",\"promptHookError\":\"" + jsonlite::escapeUtf8(info.codexPromptHookBlockReason) + "\"" +
           ",\"configPath\":\"" + jsonlite::escapeUtf8(info.codexConfigPath) +
           "\",\"hooksConfigPath\":\"" + jsonlite::escapeUtf8(info.codexHooksPath) +
           "\",\"forwardTarget\":" + (fileExists((Path(codexAdapterDirectory()) / L"forward_target.json").wstring()) ? "true" : "false") +
           "},\"chatgpt\":{\"installed\":" + (info.chatgptOnline ? "true" : "false") +
           ",\"prepared\":" + (info.chatgptPrepared ? "true" : "false") +
           ",\"extensionDir\":\"" + jsonlite::escapeUtf8(info.chatgptExtensionDirectory) +
           "\"},\"backend\":{\"python\":\"" + jsonlite::escapeUtf8(info.pythonCommand) + "\"}}";
}
