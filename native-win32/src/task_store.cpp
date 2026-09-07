#include "task_store.h"

#include <sqlite3.h>
#include <windows.h>
#include <shlobj.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <vector>

namespace {

constexpr int kMaxTitle = 512;
constexpr int kMaxPreview = 10'000;
constexpr int kMaxReply = 200'000;
constexpr int kMaxPath = 1'024;
constexpr int kMaxUrl = 2'048;
constexpr int kMaxExternalId = 128;

std::string utf8(const std::wstring &value) { return jsonlite::toUtf8(value); }
std::wstring wide(const unsigned char *value) {
    return value ? jsonlite::fromUtf8(reinterpret_cast<const char *>(value)) : std::wstring();
}

void bindText(sqlite3_stmt *statement, int index, const std::wstring &value) {
    const std::string text = utf8(value);
    sqlite3_bind_text(statement, index, text.c_str(), static_cast<int>(text.size()), SQLITE_TRANSIENT);
}

void bindNullableText(sqlite3_stmt *statement, int index, const std::wstring &value) {
    if (value.empty()) sqlite3_bind_null(statement, index);
    else bindText(statement, index, value);
}

std::wstring readColumn(sqlite3_stmt *statement, int index) {
    return wide(sqlite3_column_text(statement, index));
}

std::string errorText(sqlite3 *db) {
    return db ? sqlite3_errmsg(db) : "sqlite is not initialized";
}

std::string pathUtf8(const std::wstring &path) { return jsonlite::toUtf8(path); }

bool contains(const std::initializer_list<const wchar_t *> values, const std::wstring &value) {
    return std::any_of(values.begin(), values.end(), [&](const wchar_t *item) { return value == item; });
}

std::wstring roamingDataDirectory() {
    PWSTR appData = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, KF_FLAG_DEFAULT, nullptr, &appData)) && appData) {
        result = std::wstring(appData) + L"\\AI Task Hub";
        CoTaskMemFree(appData);
    }
    if (result.empty()) {
        wchar_t buffer[MAX_PATH]{};
        GetEnvironmentVariableW(L"APPDATA", buffer, MAX_PATH);
        if (buffer[0] != L'\0') result = std::wstring(buffer) + L"\\AI Task Hub";
    }
    return result;
}

std::wstring executableDirectory() {
    std::vector<wchar_t> buffer(512);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) return {};
        if (length < buffer.size() - 1) {
            return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path().wstring();
        }
        buffer.resize(buffer.size() * 2);
    }
}

void copyIfMissing(const std::filesystem::path &source, const std::filesystem::path &target) {
    std::error_code error;
    if (!std::filesystem::exists(source, error) || std::filesystem::exists(target, error)) return;
    error.clear();
    std::filesystem::copy_file(source, target, std::filesystem::copy_options::none, error);
}

void migrateLegacyFiles(const std::wstring &legacyDirectory, const std::wstring &portableDirectory) {
    if (legacyDirectory.empty() || portableDirectory.empty() || legacyDirectory == portableDirectory) return;
    const std::filesystem::path legacy(legacyDirectory);
    const std::filesystem::path portable(portableDirectory);
    // 只在便携目录还没有数据库时迁移，避免覆盖用户已经产生的新数据。
    std::error_code error;
    if (std::filesystem::exists(portable / L"data.sqlite", error)) return;
    copyIfMissing(legacy / L"data.sqlite", portable / L"data.sqlite");
    copyIfMissing(legacy / L"data.sqlite-wal", portable / L"data.sqlite-wal");
    copyIfMissing(legacy / L"data.sqlite-shm", portable / L"data.sqlite-shm");
}

} // namespace

TaskStore::TaskStore(std::wstring dataDirectory) {
    // 便携版默认把数据库放在 exe 同目录，下载到哪里就跟随到哪里。
    // 旧版 AppData 目录只作为首次启动迁移源，不改变已有任务和主题配置。
    legacyDataDirectory_ = dataDirectory.empty() ? roamingDataDirectory() : dataDirectory;
    dataDirectory_ = dataDirectory.empty() ? executableDirectory() : std::move(dataDirectory);
    if (dataDirectory_.empty()) dataDirectory_ = legacyDataDirectory_;
    std::error_code error;
    std::filesystem::create_directories(dataDirectory_, error);
    if (error && dataDirectory_ != legacyDataDirectory_ && !legacyDataDirectory_.empty()) {
        dataDirectory_ = legacyDataDirectory_;
        error.clear();
        std::filesystem::create_directories(dataDirectory_, error);
    }
    databasePath_ = (std::filesystem::path(dataDirectory_) / L"data.sqlite").wstring();
    migrateLegacyFiles(legacyDataDirectory_, dataDirectory_);
    initialize();
}

TaskStore::~TaskStore() {
    std::lock_guard lock(mutex_);
    if (db_) sqlite3_close_v2(db_);
    db_ = nullptr;
}

bool TaskStore::ready() const {
    std::lock_guard lock(mutex_);
    return db_ != nullptr && error_.empty();
}

std::wstring TaskStore::error() const {
    std::lock_guard lock(mutex_);
    return error_;
}

void TaskStore::setError(const std::string &message) const {
    error_ = jsonlite::fromUtf8(message);
}

void TaskStore::clearError() const { error_.clear(); }

bool TaskStore::initialize() {
    std::lock_guard lock(mutex_);
    if (sqlite3_open_v2(pathUtf8(databasePath_).c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        setError(errorText(db_));
        return false;
    }
    sqlite3_busy_timeout(db_, 5000);
    if (!exec("PRAGMA foreign_keys = ON") || !exec("PRAGMA journal_mode = WAL") ||
        !exec("PRAGMA synchronous = NORMAL") || !ensureSchema()) {
        if (error_.empty()) setError(errorText(db_));
        return false;
    }

    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, "PRAGMA table_info(task)", -1, &statement, nullptr) == SQLITE_OK) {
        while (sqlite3_step(statement) == SQLITE_ROW) {
            const std::wstring name = readColumn(statement, 1);
            if (name == L"external_id") externalColumn_ = L"external_id";
            if (name == L"external_task_id") externalColumn_ = L"external_task_id";
        }
    }
    sqlite3_finalize(statement);
    clearError();
    return true;
}

bool TaskStore::exec(const char *sql) const {
    char *message = nullptr;
    const int result = sqlite3_exec(db_, sql, nullptr, nullptr, &message);
    if (result != SQLITE_OK) {
        setError(message ? message : errorText(db_));
        sqlite3_free(message);
        return false;
    }
    return true;
}

bool TaskStore::ensureSchema() {
    // 兼容 Python 版 generated column，同时兼容上一版 native 的 external_id。
    const char *schema =
        "CREATE TABLE IF NOT EXISTS task ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, source TEXT NOT NULL, external_task_id TEXT, "
        "event_type TEXT NOT NULL, title TEXT, content_preview TEXT, project_path TEXT, open_target TEXT, "
        "open_url TEXT, status TEXT NOT NULL, created_at TEXT NOT NULL, completed_at TEXT, viewed_at TEXT, "
        "external_task_id_not_null TEXT GENERATED ALWAYS AS (IFNULL(external_task_id,'')) STORED, "
        "UNIQUE(source, external_task_id_not_null));"
        "CREATE INDEX IF NOT EXISTS idx_task_status ON task(status);"
        "CREATE INDEX IF NOT EXISTS idx_task_created_at ON task(created_at);"
        "CREATE TABLE IF NOT EXISTS task_event ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, task_id INTEGER NOT NULL, event_type TEXT NOT NULL, "
        "raw_payload TEXT, created_at TEXT NOT NULL, FOREIGN KEY(task_id) REFERENCES task(id) ON DELETE CASCADE);"
        "CREATE INDEX IF NOT EXISTS idx_task_event_task ON task_event(task_id,id);";
    if (exec(schema)) return true;
    // An old native database may already have external_id and reject the generated-column schema.
    clearError();
    return exec("CREATE INDEX IF NOT EXISTS idx_task_status ON task(status);") &&
           exec("CREATE INDEX IF NOT EXISTS idx_task_created_at ON task(created_at);") &&
           exec("CREATE TABLE IF NOT EXISTS task_event (id INTEGER PRIMARY KEY AUTOINCREMENT, task_id INTEGER NOT NULL, event_type TEXT NOT NULL, raw_payload TEXT, created_at TEXT NOT NULL, FOREIGN KEY(task_id) REFERENCES task(id) ON DELETE CASCADE);") &&
           exec("CREATE INDEX IF NOT EXISTS idx_task_event_task ON task_event(task_id,id);");
}

std::wstring TaskStore::columnName() const { return externalColumn_; }

std::wstring TaskStore::setting(const wchar_t *key) const {
    // 读取当前目录和旧 AppData 目录的配置，确保便携迁移后壁纸/头像/主题不丢失。
    const auto readFrom = [&](const std::wstring &directory) {
        wchar_t buffer[2048]{};
        const std::wstring ini = directory + L"\\AI Task Hub.ini";
        GetPrivateProfileStringW(L"General", key, L"", buffer, static_cast<DWORD>(std::size(buffer)), ini.c_str());
        if (buffer[0] == L'\0') {
            const std::wstring legacyIni = directory + L"\\settings.ini";
            GetPrivateProfileStringW(L"appearance", key, L"", buffer, static_cast<DWORD>(std::size(buffer)), legacyIni.c_str());
        }
        return std::wstring(buffer);
    };
    std::wstring value = readFrom(dataDirectory_);
    if (value.empty() && legacyDataDirectory_ != dataDirectory_) value = readFrom(legacyDataDirectory_);
    return value;
}

void TaskStore::writeSetting(const wchar_t *key, const std::wstring &value) const {
    const std::wstring ini = dataDirectory_ + L"\\AI Task Hub.ini";
    WritePrivateProfileStringW(L"General", key, value.c_str(), ini.c_str());
}

std::wstring TaskStore::themeId() const {
    const std::wstring value = setting(L"themeId");
    return contains({L"default", L"rei-ayanami", L"tomo-ebizuka", L"elaina", L"mutsumi-wakaba",
                     L"sakiko-togawa", L"yui-hirasawa", L"mio-akiyama", L"ritsu-tainaka",
                     L"tsumugi-kotobuki", L"azusa-nakano"}, value)
               ? value
               : L"default";
}

std::wstring TaskStore::userIconPreset() const {
    const std::wstring value = setting(L"userIconPreset");
    if (contains({L"default", L"rei-ayanami", L"tomo-ebizuka", L"elaina", L"mutsumi-wakaba",
                     L"sakiko-togawa", L"yui-hirasawa", L"mio-akiyama", L"ritsu-tainaka",
                     L"tsumugi-kotobuki", L"azusa-nakano"}, value))
        return value;
    const std::wstring configured = setting(L"userIconPath");
    const std::wstring marker = L"icon-preset-";
    const size_t start = configured.find(marker);
    if (start != std::wstring::npos) {
        const size_t end = configured.find(L'.', start + marker.size());
        const std::wstring inferred = configured.substr(start + marker.size(), end == std::wstring::npos ? std::wstring::npos : end - start - marker.size());
        if (contains({L"default", L"rei-ayanami", L"tomo-ebizuka", L"elaina", L"mutsumi-wakaba",
                      L"sakiko-togawa", L"yui-hirasawa", L"mio-akiyama", L"ritsu-tainaka",
                      L"tsumugi-kotobuki", L"azusa-nakano"}, inferred)) return inferred;
    }
    return L"default";
}

std::wstring TaskStore::wallpaperPath() const {
    const std::wstring path = setting(L"wallpaperPath");
    return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ? L"" : path;
}

std::wstring TaskStore::userIconPath() const {
    const std::wstring path = setting(L"userIconPath");
    return GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES ? L"" : path;
}

int TaskStore::windowOpacityPercent() const {
    const std::wstring value = setting(L"windowOpacity");
    if (value.empty()) return 100;
    try {
        return std::clamp(std::stoi(value), 60, 100);
    } catch (...) {
        return 100;
    }
}

int TaskStore::backgroundBlurLevel() const {
    const std::wstring value = setting(L"backgroundBlur");
    if (value.empty()) return 0;
    try {
        return std::clamp(std::stoi(value), 0, 2);
    } catch (...) {
        return 0;
    }
}

std::wstring TaskStore::nowIso() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t value[64]{};
    swprintf_s(value, L"%04u-%02u-%02uT%02u:%02u:%02u.%03u", time.wYear, time.wMonth, time.wDay,
               time.wHour, time.wMinute, time.wSecond, time.wMilliseconds);
    return value;
}

std::wstring TaskStore::bounded(const jsonlite::Value *value, size_t maxLength) {
    if (!value || !value->isString()) return {};
    std::wstring result = value->string();
    while (!result.empty() && iswspace(result.back())) result.pop_back();
    size_t first = 0;
    while (first < result.size() && iswspace(result[first])) ++first;
    if (first > 0) result.erase(0, first);
    if (result.size() > maxLength) result.resize(maxLength);
    return result;
}

std::wstring TaskStore::deriveTitle(const std::wstring &preview) {
    std::wstring oneLine = preview;
    for (auto &ch : oneLine) if (ch == L'\r' || ch == L'\n' || ch == L'\t') ch = L' ';
    while (oneLine.find(L"  ") != std::wstring::npos) oneLine.replace(oneLine.find(L"  "), 2, L" ");
    if (oneLine.empty()) return L"未命名任务";
    return oneLine.substr(0, 50) + (oneLine.size() > 50 ? L"…" : L"");
}

std::wstring TaskStore::statusFor(const std::wstring &eventType) {
    if (eventType == L"TASK_NEEDS_INPUT") return L"NEEDS_INPUT";
    if (eventType == L"TASK_COMPLETED") return L"COMPLETED_UNREAD";
    if (eventType == L"TASK_FAILED") return L"FAILED_UNREAD";
    if (eventType == L"TASK_VIEWED") return L"VIEWED";
    if (eventType == L"TASK_IGNORED") return L"IGNORED";
    return L"RUNNING";
}

bool TaskStore::isAllowedSource(const std::wstring &source) {
    return contains({L"CHATGPT", L"CLAUDE_CODE", L"CODEX", L"OTHER"}, source);
}

bool TaskStore::isAllowedEventType(const std::wstring &eventType) {
    return contains({L"TASK_STARTED", L"TASK_NEEDS_INPUT", L"TASK_COMPLETED", L"TASK_FAILED",
                     L"TASK_VIEWED", L"TASK_IGNORED"}, eventType);
}

bool TaskStore::isAllowedStatus(const std::wstring &status) {
    return contains({L"VIEWED", L"IGNORED"}, status);
}

HubTask TaskStore::readTask(void *rawStatement) const {
    auto *statement = static_cast<sqlite3_stmt *>(rawStatement);
    HubTask task;
    task.id = sqlite3_column_int64(statement, 0);
    task.source = readColumn(statement, 1);
    task.externalTaskId = readColumn(statement, 2);
    task.eventType = readColumn(statement, 3);
    task.title = readColumn(statement, 4);
    task.contentPreview = readColumn(statement, 5);
    task.projectPath = readColumn(statement, 6);
    task.openTarget = readColumn(statement, 7);
    task.openUrl = readColumn(statement, 8);
    task.status = readColumn(statement, 9);
    task.createdAt = readColumn(statement, 10);
    task.completedAt = readColumn(statement, 11);
    task.viewedAt = readColumn(statement, 12);
    return task;
}

std::vector<HubTask> TaskStore::tasks(const std::wstring &view, const std::wstring &status,
                                      const std::wstring &source, const std::wstring &search,
                                      int limit, int offset, bool *hasMore) const {
    std::lock_guard lock(mutex_);
    if (hasMore) *hasMore = false;
    std::vector<HubTask> result;
    if (!db_) return result;
    const int safeLimit = std::clamp(limit, 1, 500);
    std::string sql = "SELECT id," + pathUtf8(L"source") + "," + pathUtf8(externalColumn_) + ",event_type,title,content_preview,project_path,open_target,open_url,status,created_at,completed_at,viewed_at FROM task WHERE 1=1";
    if (view == L"queue") sql += " AND status IN ('RUNNING','NEEDS_INPUT','COMPLETED_UNREAD','FAILED_UNREAD')";
    if (view == L"history") sql += " AND status IN ('VIEWED','IGNORED')";
    if (!status.empty() && status != L"ALL") { sql += " AND status = ?"; }
    if (!source.empty() && source != L"ALL") { sql += " AND source = ?"; }
    if (!search.empty()) sql += " AND (title LIKE ? OR content_preview LIKE ? OR project_path LIKE ?)";
    sql += " ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?";
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) return result;
    int bindIndex = 1;
    if (!status.empty() && status != L"ALL") bindText(statement, bindIndex++, status);
    if (!source.empty() && source != L"ALL") bindText(statement, bindIndex++, source);
    if (!search.empty()) {
        const std::wstring pattern = L"%" + search + L"%";
        bindText(statement, bindIndex++, pattern);
        bindText(statement, bindIndex++, pattern);
        bindText(statement, bindIndex++, pattern);
    }
    sqlite3_bind_int(statement, bindIndex++, safeLimit + 1);
    sqlite3_bind_int(statement, bindIndex, std::max(0, offset));
    while (sqlite3_step(statement) == SQLITE_ROW) result.push_back(readTask(statement));
    sqlite3_finalize(statement);
    if (hasMore && static_cast<int>(result.size()) > safeLimit) {
        *hasMore = true;
        result.resize(static_cast<size_t>(safeLimit));
    }
    return result;
}

HubSnapshot TaskStore::snapshot(int limit) const {
    HubSnapshot snapshot;
    snapshot.queue = tasks(L"queue", {}, {}, {}, limit, 0);
    snapshot.history = tasks(L"history", {}, {}, {}, limit, 0);
    std::lock_guard lock(mutex_);
    if (!db_) return snapshot;
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT status,COUNT(*) FROM task GROUP BY status", -1, &statement, nullptr) == SQLITE_OK) {
        while (sqlite3_step(statement) == SQLITE_ROW) {
            const std::wstring status = readColumn(statement, 0);
            const int count = sqlite3_column_int(statement, 1);
            if (status == L"RUNNING") snapshot.counts.running = count;
            else if (status == L"NEEDS_INPUT") snapshot.counts.needsInput = count;
            else if (status == L"COMPLETED_UNREAD") snapshot.counts.completedUnread = count;
            else if (status == L"FAILED_UNREAD") snapshot.counts.failedUnread = count;
            else if (status == L"VIEWED") snapshot.counts.viewed = count;
            else if (status == L"IGNORED") snapshot.counts.ignored = count;
        }
    }
    sqlite3_finalize(statement);
    snapshot.counts.total = snapshot.counts.running + snapshot.counts.needsInput + snapshot.counts.completedUnread + snapshot.counts.failedUnread + snapshot.counts.viewed + snapshot.counts.ignored;
    snapshot.counts.queue = snapshot.counts.running + snapshot.counts.needsInput + snapshot.counts.completedUnread + snapshot.counts.failedUnread;
    snapshot.counts.history = snapshot.counts.viewed + snapshot.counts.ignored;
    return snapshot;
}

bool TaskStore::task(std::int64_t id, HubTask &result) const {
    std::lock_guard lock(mutex_);
    if (!db_ || id <= 0) return false;
    const std::string sql = "SELECT id,source," + pathUtf8(externalColumn_) + ",event_type,title,content_preview,project_path,open_target,open_url,status,created_at,completed_at,viewed_at FROM task WHERE id=?";
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(statement, 1, id);
    const bool found = sqlite3_step(statement) == SQLITE_ROW;
    if (found) result = readTask(statement);
    sqlite3_finalize(statement);
    return found;
}

std::vector<HubEvent> TaskStore::events(std::int64_t id) const {
    std::lock_guard lock(mutex_);
    std::vector<HubEvent> result;
    if (!db_ || id <= 0) return result;
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT id,task_id,event_type,raw_payload,created_at FROM task_event WHERE task_id=? ORDER BY id ASC", -1, &statement, nullptr) != SQLITE_OK) return result;
    sqlite3_bind_int64(statement, 1, id);
    while (sqlite3_step(statement) == SQLITE_ROW) {
        HubEvent event;
        event.id = sqlite3_column_int64(statement, 0);
        event.taskId = sqlite3_column_int64(statement, 1);
        event.eventType = readColumn(statement, 2);
        event.rawPayload = readColumn(statement, 3);
        event.createdAt = readColumn(statement, 4);
        result.push_back(std::move(event));
    }
    sqlite3_finalize(statement);
    return result;
}

std::wstring TaskStore::aiReply(std::int64_t id) const {
    const auto allEvents = events(id);
    std::wstring result;
    for (const auto &event : allEvents) {
        jsonlite::Value payload;
        std::string error;
        if (!jsonlite::parseUtf8(utf8(event.rawPayload), payload, error)) continue;
        if (const auto *reply = payload.get(L"replyText")) {
            const std::wstring text = reply->string();
            if (!text.empty()) result = text;
        }
    }
    return result;
}

std::int64_t TaskStore::ingest(const jsonlite::Value &event) {
    const std::wstring source = bounded(event.get(L"source"), 32);
    const std::wstring eventType = bounded(event.get(L"eventType"), 64);
    if (!isAllowedSource(source) || !isAllowedEventType(eventType)) {
        std::lock_guard lock(mutex_);
        setError("source or eventType is outside AgentEvent contract");
        return 0;
    }
    const std::wstring externalId = bounded(event.get(L"externalTaskId"), kMaxExternalId);
    const std::wstring titleInput = bounded(event.get(L"title"), kMaxTitle);
    const std::wstring preview = bounded(event.get(L"contentPreview"), kMaxPreview);
    const std::wstring reply = bounded(event.get(L"replyText"), kMaxReply);
    const std::wstring projectPath = bounded(event.get(L"projectPath"), kMaxPath);
    const std::wstring openUrl = bounded(event.get(L"openUrl"), kMaxUrl);
    const std::wstring openTarget = bounded(event.get(L"openTarget"), 32);
    if (!openTarget.empty() && !contains({L"browser", L"terminal", L"none"}, openTarget)) {
        std::lock_guard lock(mutex_);
        setError("openTarget must be browser, terminal or none");
        return 0;
    }
    const std::wstring title = titleInput.empty() ? deriveTitle(preview) : titleInput;
    std::wstring createdAt = bounded(event.get(L"createdAt"), 64);
    if (createdAt.empty()) createdAt = nowIso();
    const std::wstring status = statusFor(eventType);

    jsonlite::Value normalized = event;
    if (normalized.isObject()) {
        auto object = std::get<jsonlite::Value::Object>(normalized.data);
        object[L"title"] = jsonlite::Value(title);
        if (!reply.empty()) object[L"replyText"] = jsonlite::Value(reply);
        normalized = jsonlite::Value(std::move(object));
    }
    const std::string rawPayload = jsonlite::stringifyUtf8(normalized);
    std::lock_guard lock(mutex_);
    if (!db_) return 0;
    if (!exec("BEGIN IMMEDIATE")) return 0;
    const std::string findSql = "SELECT id FROM task WHERE source=? AND IFNULL(" + pathUtf8(externalColumn_) + ",'')=? LIMIT 1";
    sqlite3_stmt *find = nullptr;
    std::int64_t id = 0;
    if (sqlite3_prepare_v2(db_, findSql.c_str(), -1, &find, nullptr) == SQLITE_OK) {
        bindText(find, 1, source); bindText(find, 2, externalId);
        if (sqlite3_step(find) == SQLITE_ROW) id = sqlite3_column_int64(find, 0);
    }
    sqlite3_finalize(find);
    sqlite3_stmt *statement = nullptr;
    bool ok = false;
    if (id == 0) {
        const std::string sql = "INSERT INTO task(source," + pathUtf8(externalColumn_) + ",event_type,title,content_preview,project_path,open_target,open_url,status,created_at,completed_at,viewed_at) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK) {
            bindText(statement, 1, source); bindNullableText(statement, 2, externalId); bindText(statement, 3, eventType);
            bindText(statement, 4, title); bindText(statement, 5, preview); bindText(statement, 6, projectPath);
            bindText(statement, 7, openTarget); bindText(statement, 8, openUrl); bindText(statement, 9, status); bindText(statement, 10, createdAt);
            if (status == L"COMPLETED_UNREAD" || status == L"FAILED_UNREAD") bindText(statement, 11, createdAt); else sqlite3_bind_null(statement, 11);
            if (status == L"VIEWED") bindText(statement, 12, createdAt); else sqlite3_bind_null(statement, 12);
            ok = sqlite3_step(statement) == SQLITE_DONE;
            if (ok) id = sqlite3_last_insert_rowid(db_);
        }
    }
    else {
        const std::string sql = "UPDATE task SET event_type=?,title=CASE WHEN ?<>'' THEN ? ELSE title END,content_preview=CASE WHEN ?<>'' THEN ? ELSE content_preview END,project_path=CASE WHEN ?<>'' THEN ? ELSE project_path END,open_target=CASE WHEN ?<>'' THEN ? ELSE open_target END,open_url=CASE WHEN ?<>'' THEN ? ELSE open_url END,status=?,completed_at=CASE WHEN ? IN ('COMPLETED_UNREAD','FAILED_UNREAD') THEN ? ELSE completed_at END,viewed_at=CASE WHEN ?='VIEWED' THEN ? WHEN ? IN ('RUNNING','NEEDS_INPUT','COMPLETED_UNREAD','FAILED_UNREAD') THEN NULL ELSE viewed_at END WHERE id=?";
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK) {
            int i = 1;
            bindText(statement, i++, eventType); bindText(statement, i++, titleInput); bindText(statement, i++, titleInput);
            bindText(statement, i++, preview); bindText(statement, i++, preview); bindText(statement, i++, projectPath); bindText(statement, i++, projectPath);
            bindText(statement, i++, openTarget); bindText(statement, i++, openTarget); bindText(statement, i++, openUrl); bindText(statement, i++, openUrl);
            bindText(statement, i++, status); bindText(statement, i++, status); bindText(statement, i++, createdAt); bindText(statement, i++, status); bindText(statement, i++, createdAt); bindText(statement, i++, status); sqlite3_bind_int64(statement, i, id);
            ok = sqlite3_step(statement) == SQLITE_DONE;
        }
    }
    sqlite3_finalize(statement);
    if (ok) {
        sqlite3_stmt *eventStatement = nullptr;
        if (sqlite3_prepare_v2(db_, "INSERT INTO task_event(task_id,event_type,raw_payload,created_at) VALUES(?,?,?,?)", -1, &eventStatement, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(eventStatement, 1, id); bindText(eventStatement, 2, eventType); bindText(eventStatement, 3, jsonlite::fromUtf8(rawPayload)); bindText(eventStatement, 4, createdAt);
            ok = sqlite3_step(eventStatement) == SQLITE_DONE;
        }
        sqlite3_finalize(eventStatement);
    }
    if (ok) ok = exec("COMMIT"); else exec("ROLLBACK");
    if (!ok) {
        if (error_.empty()) setError(errorText(db_));
        return 0;
    }
    clearError();
    notifyChanged();
    return id;
}

bool TaskStore::setStatus(std::int64_t id, const std::wstring &status) {
    if (!isAllowedStatus(status)) return false;
    const std::wstring eventType = status == L"VIEWED" ? L"TASK_VIEWED" : L"TASK_IGNORED";
    const std::wstring timestamp = nowIso();
    std::lock_guard lock(mutex_);
    if (!db_ || !exec("BEGIN IMMEDIATE")) return false;
    sqlite3_stmt *statement = nullptr;
    bool ok = sqlite3_prepare_v2(db_, "UPDATE task SET status=?,event_type=?,viewed_at=? WHERE id=?", -1, &statement, nullptr) == SQLITE_OK;
    if (ok) { bindText(statement, 1, status); bindText(statement, 2, eventType); if (status == L"VIEWED") bindText(statement, 3, timestamp); else sqlite3_bind_null(statement, 3); sqlite3_bind_int64(statement, 4, id); ok = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(db_) > 0; }
    sqlite3_finalize(statement);
    statement = nullptr;
    if (ok) {
        if (sqlite3_prepare_v2(db_, "INSERT INTO task_event(task_id,event_type,raw_payload,created_at) VALUES(?,?,?,?)", -1, &statement, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(statement, 1, id); bindText(statement, 2, eventType); bindText(statement, 3, L"{\"eventType\":\"" + eventType + L"\"}"); bindText(statement, 4, timestamp); ok = sqlite3_step(statement) == SQLITE_DONE;
        } else ok = false;
    }
    sqlite3_finalize(statement);
    if (ok) ok = exec("COMMIT"); else exec("ROLLBACK");
    if (ok) notifyChanged();
    return ok;
}

bool TaskStore::remove(std::int64_t id) {
    std::lock_guard lock(mutex_);
    if (!db_) return false;
    sqlite3_stmt *statement = nullptr;
    const bool prepared = sqlite3_prepare_v2(db_, "DELETE FROM task WHERE id=?", -1, &statement, nullptr) == SQLITE_OK;
    bool ok = false;
    if (prepared) {
        sqlite3_bind_int64(statement, 1, id);
        ok = sqlite3_step(statement) == SQLITE_DONE && sqlite3_changes(db_) > 0;
    }
    sqlite3_finalize(statement);
    if (ok) notifyChanged();
    return ok;
}

int TaskStore::clear(const std::wstring &scope) {
    std::lock_guard lock(mutex_);
    if (!db_) return 0;
    // 清理范围只接受白名单，避免未知 scope 意外退化为“清空全部”。
    std::wstring view;
    std::wstring source;
    if (scope == L"all") {
        // no condition
    } else if (scope == L"queue" || scope == L"history") {
        view = scope;
    } else if (scope.rfind(L"source:", 0) == 0) {
        source = scope.substr(7);
        if (source.empty()) return 0;
    } else if (scope.rfind(L"queue:source:", 0) == 0) {
        view = L"queue";
        source = scope.substr(13);
        if (source.empty()) return 0;
    } else if (scope.rfind(L"history:source:", 0) == 0) {
        view = L"history";
        source = scope.substr(15);
        if (source.empty()) return 0;
    } else {
        return 0;
    }
    if (!source.empty() && !isAllowedSource(source)) return 0;

    std::string sql = "DELETE FROM task";
    bool hasWhere = false;
    if (view == L"queue") {
        sql += " WHERE status IN ('RUNNING','NEEDS_INPUT','COMPLETED_UNREAD','FAILED_UNREAD')";
        hasWhere = true;
    } else if (view == L"history") {
        sql += " WHERE status IN ('VIEWED','IGNORED')";
        hasWhere = true;
    }
    if (!source.empty()) {
        sql += hasWhere ? " AND source=?" : " WHERE source=?";
    }
    sqlite3_stmt *statement = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) return 0;
    if (!source.empty()) bindText(statement, 1, source);
    const bool deleted = sqlite3_step(statement) == SQLITE_DONE;
    const int count = deleted ? sqlite3_changes(db_) : 0;
    sqlite3_finalize(statement);
    if (count > 0) notifyChanged();
    return count;
}

int TaskStore::markAllViewed() {
    std::lock_guard lock(mutex_);
    if (!db_) return 0;
    const std::wstring timestamp = nowIso();
    sqlite3_stmt *ids = nullptr;
    std::vector<std::int64_t> taskIds;
    if (sqlite3_prepare_v2(db_, "SELECT id FROM task WHERE status IN ('COMPLETED_UNREAD','FAILED_UNREAD')", -1, &ids, nullptr) == SQLITE_OK) {
        while (sqlite3_step(ids) == SQLITE_ROW) taskIds.push_back(sqlite3_column_int64(ids, 0));
    }
    sqlite3_finalize(ids);
    if (taskIds.empty()) return 0;
    if (!exec("BEGIN IMMEDIATE") || sqlite3_exec(db_, "UPDATE task SET status='VIEWED',event_type='TASK_VIEWED',viewed_at=datetime('now') WHERE status IN ('COMPLETED_UNREAD','FAILED_UNREAD')", nullptr, nullptr, nullptr) != SQLITE_OK) { exec("ROLLBACK"); return 0; }
    sqlite3_stmt *event = nullptr;
    if (sqlite3_prepare_v2(db_, "INSERT INTO task_event(task_id,event_type,raw_payload,created_at) VALUES(?,?,?,?)", -1, &event, nullptr) != SQLITE_OK) { exec("ROLLBACK"); return 0; }
    for (const auto id : taskIds) {
        sqlite3_reset(event); sqlite3_clear_bindings(event); sqlite3_bind_int64(event, 1, id); bindText(event, 2, L"TASK_VIEWED"); bindText(event, 3, L"{\"eventType\":\"TASK_VIEWED\"}"); bindText(event, 4, timestamp);
        if (sqlite3_step(event) != SQLITE_DONE) { sqlite3_finalize(event); exec("ROLLBACK"); return 0; }
    }
    sqlite3_finalize(event);
    if (!exec("COMMIT")) return 0;
    notifyChanged();
    return static_cast<int>(taskIds.size());
}

bool TaskStore::darkMode() const { return setting(L"darkMode") != L"false"; }
bool TaskStore::notificationsEnabled() const { return setting(L"notificationsEnabled") != L"false"; }
void TaskStore::setDarkMode(bool enabled) { writeSetting(L"darkMode", enabled ? L"true" : L"false"); }
void TaskStore::setNotificationsEnabled(bool enabled) { writeSetting(L"notificationsEnabled", enabled ? L"true" : L"false"); }

bool TaskStore::setTheme(const std::wstring &theme) {
    if (!contains({L"default", L"rei-ayanami", L"tomo-ebizuka", L"elaina", L"mutsumi-wakaba",
                   L"sakiko-togawa", L"yui-hirasawa", L"mio-akiyama", L"ritsu-tainaka",
                   L"tsumugi-kotobuki", L"azusa-nakano"}, theme)) return false;
    writeSetting(L"themeId", theme);
    notifyChanged();
    return true;
}

bool TaskStore::setUserIconPreset(const std::wstring &preset) {
    if (!contains({L"default", L"rei-ayanami", L"tomo-ebizuka", L"elaina", L"mutsumi-wakaba",
                   L"sakiko-togawa", L"yui-hirasawa", L"mio-akiyama", L"ritsu-tainaka",
                   L"tsumugi-kotobuki", L"azusa-nakano"}, preset)) return false;
    writeSetting(L"userIconPath", L"");
    writeSetting(L"userIconPreset", preset);
    notifyChanged();
    return true;
}

bool TaskStore::setWallpaper(const std::wstring &path) {
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    writeSetting(L"wallpaperPath", path);
    notifyChanged();
    return true;
}

bool TaskStore::setUserIcon(const std::wstring &path) {
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    writeSetting(L"userIconPath", path);
    writeSetting(L"userIconPreset", L"");
    notifyChanged();
    return true;
}

bool TaskStore::setWindowOpacityPercent(int percent) {
    if (percent < 60 || percent > 100) return false;
    writeSetting(L"windowOpacity", std::to_wstring(percent));
    notifyChanged();
    return true;
}

bool TaskStore::setBackgroundBlurLevel(int level) {
    if (level < 0 || level > 2) return false;
    writeSetting(L"backgroundBlur", std::to_wstring(level));
    notifyChanged();
    return true;
}

void TaskStore::clearWallpaper() { writeSetting(L"wallpaperPath", L""); notifyChanged(); }
void TaskStore::clearUserIcon() { writeSetting(L"userIconPath", L""); writeSetting(L"userIconPreset", L""); notifyChanged(); }

void TaskStore::setChangeCallback(void (*callback)(void *), void *context) {
    std::lock_guard lock(mutex_);
    changeCallback_ = callback;
    changeContext_ = context;
}

void TaskStore::notifyChanged() {
    // 读写线程只通知窗口，避免回调中再次进入 SQLite 锁。
    if (changeCallback_) changeCallback_(changeContext_);
}

std::string taskToJson(const HubTask &task) {
    return "{\"id\":" + std::to_string(task.id) + ",\"source\":\"" + jsonlite::escapeUtf8(task.source) +
           "\",\"externalTaskId\":\"" + jsonlite::escapeUtf8(task.externalTaskId) + "\",\"eventType\":\"" + jsonlite::escapeUtf8(task.eventType) +
           "\",\"title\":\"" + jsonlite::escapeUtf8(task.title) + "\",\"contentPreview\":\"" + jsonlite::escapeUtf8(task.contentPreview) +
           "\",\"projectPath\":\"" + jsonlite::escapeUtf8(task.projectPath) + "\",\"openTarget\":\"" + jsonlite::escapeUtf8(task.openTarget) +
           "\",\"openUrl\":\"" + jsonlite::escapeUtf8(task.openUrl) + "\",\"status\":\"" + jsonlite::escapeUtf8(task.status) +
           "\",\"createdAt\":\"" + jsonlite::escapeUtf8(task.createdAt) + "\",\"completedAt\":\"" + jsonlite::escapeUtf8(task.completedAt) +
           "\",\"viewedAt\":\"" + jsonlite::escapeUtf8(task.viewedAt) + "\"}";
}

std::string eventToJson(const HubEvent &event) {
    return "{\"id\":" + std::to_string(event.id) + ",\"taskId\":" + std::to_string(event.taskId) +
           ",\"eventType\":\"" + jsonlite::escapeUtf8(event.eventType) + "\",\"occurredAt\":\"" + jsonlite::escapeUtf8(event.createdAt) +
           "\",\"payload\":" + jsonlite::toUtf8(event.rawPayload.empty() ? L"{}" : event.rawPayload) + "}";
}

std::string countsToJson(const HubCounts &counts) {
    return "{\"RUNNING\":" + std::to_string(counts.running) + ",\"NEEDS_INPUT\":" + std::to_string(counts.needsInput) +
           ",\"COMPLETED_UNREAD\":" + std::to_string(counts.completedUnread) + ",\"FAILED_UNREAD\":" + std::to_string(counts.failedUnread) +
           ",\"VIEWED\":" + std::to_string(counts.viewed) + ",\"IGNORED\":" + std::to_string(counts.ignored) +
           ",\"total\":" + std::to_string(counts.total) + ",\"queue\":" + std::to_string(counts.queue) + ",\"history\":" + std::to_string(counts.history) + "}";
}
