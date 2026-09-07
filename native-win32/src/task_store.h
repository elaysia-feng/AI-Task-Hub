#pragma once

#include "json_lite.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

struct HubTask {
    std::int64_t id = 0;
    std::wstring source;
    std::wstring externalTaskId;
    std::wstring eventType;
    std::wstring title;
    std::wstring contentPreview;
    std::wstring projectPath;
    std::wstring openTarget;
    std::wstring openUrl;
    std::wstring status;
    std::wstring createdAt;
    std::wstring completedAt;
    std::wstring viewedAt;
};

struct HubEvent {
    std::int64_t id = 0;
    std::int64_t taskId = 0;
    std::wstring eventType;
    std::wstring rawPayload;
    std::wstring createdAt;
};

struct HubCounts {
    int running = 0;
    int needsInput = 0;
    int completedUnread = 0;
    int failedUnread = 0;
    int viewed = 0;
    int ignored = 0;
    int total = 0;
    int queue = 0;
    int history = 0;
};

struct HubSnapshot {
    HubCounts counts;
    std::vector<HubTask> queue;
    std::vector<HubTask> history;
};

class TaskStore final {
public:
    explicit TaskStore(std::wstring dataDirectory = {});
    ~TaskStore();

    TaskStore(const TaskStore &) = delete;
    TaskStore &operator=(const TaskStore &) = delete;

    bool ready() const;
    std::wstring error() const;
    const std::wstring &databasePath() const { return databasePath_; }
    const std::wstring &dataDirectory() const { return dataDirectory_; }
    std::wstring themeId() const;
    std::wstring userIconPreset() const;
    std::wstring wallpaperPath() const;
    std::wstring userIconPath() const;
    int windowOpacityPercent() const;
    int backgroundBlurLevel() const;
    bool darkMode() const;
    bool notificationsEnabled() const;
    void setDarkMode(bool enabled);
    void setNotificationsEnabled(bool enabled);

    HubSnapshot snapshot(int limit = 200) const;
    std::vector<HubTask> tasks(const std::wstring &view, const std::wstring &status,
                               const std::wstring &source, const std::wstring &search,
                               int limit, int offset, bool *hasMore = nullptr) const;
    bool task(std::int64_t id, HubTask &result) const;
    std::vector<HubEvent> events(std::int64_t id) const;
    std::wstring aiReply(std::int64_t id) const;

    std::int64_t ingest(const jsonlite::Value &event);
    bool setStatus(std::int64_t id, const std::wstring &status);
    bool remove(std::int64_t id);
    int clear(const std::wstring &scope);
    int markAllViewed();

    bool setTheme(const std::wstring &theme);
    bool setUserIconPreset(const std::wstring &preset);
    bool setWallpaper(const std::wstring &path);
    bool setUserIcon(const std::wstring &path);
    bool setWindowOpacityPercent(int percent);
    bool setBackgroundBlurLevel(int level);
    void clearWallpaper();
    void clearUserIcon();

    // HTTP 事件到达时由窗口线程刷新列表；回调只做 PostMessage，不触碰 UI。
    void setChangeCallback(void (*callback)(void *), void *context);

private:
    bool initialize();
    bool ensureSchema();
    bool exec(const char *sql) const;
    std::wstring columnName() const;
    std::wstring setting(const wchar_t *key) const;
    void writeSetting(const wchar_t *key, const std::wstring &value) const;
    void setError(const std::string &message) const;
    void clearError() const;
    void notifyChanged();

    HubTask readTask(void *statement) const;
    static std::wstring nowIso();
    static std::wstring bounded(const jsonlite::Value *value, size_t maxLength);
    static std::wstring deriveTitle(const std::wstring &preview);
    static std::wstring statusFor(const std::wstring &eventType);
    static bool isAllowedSource(const std::wstring &source);
    static bool isAllowedEventType(const std::wstring &eventType);
    static bool isAllowedStatus(const std::wstring &status);

    mutable std::mutex mutex_;
    sqlite3 *db_ = nullptr;
    std::wstring dataDirectory_;
    std::wstring legacyDataDirectory_;
    std::wstring databasePath_;
    std::wstring externalColumn_ = L"external_task_id";
    mutable std::wstring error_;
    void (*changeCallback_)(void *) = nullptr;
    void *changeContext_ = nullptr;
};

std::string taskToJson(const HubTask &task);
std::string eventToJson(const HubEvent &event);
std::string countsToJson(const HubCounts &counts);
