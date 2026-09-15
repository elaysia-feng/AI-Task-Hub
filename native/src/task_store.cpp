#include "task_store.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUrl>
#include <QWindow>

#include <algorithm>

namespace {

constexpr int kRefreshLimit = 200;
constexpr int kMaxBodyBytes = 1024 * 1024;
constexpr int kMaxTitle = 512;
constexpr int kMaxPreview = 10'000;
constexpr int kMaxReply = 200'000;
constexpr int kMaxPath = 1024;
constexpr int kMaxUrl = 2'048;
constexpr int kMaxExternalId = 128;

const QStringList kStatuses{
    QStringLiteral("RUNNING"), QStringLiteral("NEEDS_INPUT"),
    QStringLiteral("COMPLETED_UNREAD"), QStringLiteral("FAILED_UNREAD"),
    QStringLiteral("VIEWED"), QStringLiteral("IGNORED")};
const QStringList kSources{QStringLiteral("CHATGPT"), QStringLiteral("CLAUDE_CODE"),
                           QStringLiteral("CODEX"), QStringLiteral("OTHER")};
const QStringList kEventTypes{
    QStringLiteral("TASK_STARTED"), QStringLiteral("TASK_NEEDS_INPUT"),
    QStringLiteral("TASK_COMPLETED"), QStringLiteral("TASK_FAILED"),
    QStringLiteral("TASK_VIEWED"), QStringLiteral("TASK_IGNORED")};
const QStringList kThemes{QStringLiteral("default"), QStringLiteral("rei-ayanami"),
                          QStringLiteral("tomo-ebizuka"), QStringLiteral("elaina"),
                          QStringLiteral("mutsumi-wakaba"), QStringLiteral("sakiko-togawa"),
                          QStringLiteral("yui-hirasawa"), QStringLiteral("mio-akiyama"),
                          QStringLiteral("ritsu-tainaka"), QStringLiteral("tsumugi-kotobuki"),
                          QStringLiteral("azusa-nakano"), QStringLiteral("ayaka-kamisato"),
                          QStringLiteral("aemeath"), QStringLiteral("shorekeeper")};

QString bundledPresetPath(const QString &presetId) {
    const QString relative = QDir(QStringLiteral("presets")).filePath(presetId + QStringLiteral(".png"));
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList roots{
        QDir(appDir).filePath(QStringLiteral("resources")),
        QDir(appDir).filePath(QStringLiteral("../desktop/resources")),
        QDir(appDir).filePath(QStringLiteral("../../desktop/resources"))};
    for (const QString &root : roots) {
        const QString candidate = QDir::cleanPath(QDir(root).filePath(relative));
        if (QFileInfo::exists(candidate)) return candidate;
    }
    return {};
}

QString statusFor(const QString &eventType) {
    if (eventType == QStringLiteral("TASK_NEEDS_INPUT")) return QStringLiteral("NEEDS_INPUT");
    if (eventType == QStringLiteral("TASK_COMPLETED")) return QStringLiteral("COMPLETED_UNREAD");
    if (eventType == QStringLiteral("TASK_FAILED")) return QStringLiteral("FAILED_UNREAD");
    if (eventType == QStringLiteral("TASK_VIEWED")) return QStringLiteral("VIEWED");
    if (eventType == QStringLiteral("TASK_IGNORED")) return QStringLiteral("IGNORED");
    return QStringLiteral("RUNNING");
}

bool contains(const QStringList &values, const QString &value) { return values.contains(value); }

QString nowIso() { return QDateTime::currentDateTime().toString(Qt::ISODateWithMs); }

QString bounded(const QJsonValue &value, int maxLength) {
    return value.toString().trimmed().left(maxLength);
}

QString deriveTitle(const QString &preview) {
    const QString oneLine = preview.simplified();
    if (oneLine.isEmpty()) return QStringLiteral("未命名任务");
    return oneLine.left(50) + (oneLine.size() > 50 ? QStringLiteral("…") : QString());
}

QVariantMap taskMap(const QSqlQuery &query) {
    return {{QStringLiteral("id"), query.value(0)},
            {QStringLiteral("source"), query.value(1)},
            {QStringLiteral("externalTaskId"), query.value(2)},
            {QStringLiteral("eventType"), query.value(3)},
            {QStringLiteral("title"), query.value(4)},
            {QStringLiteral("contentPreview"), query.value(5)},
            {QStringLiteral("projectPath"), query.value(6)},
            {QStringLiteral("openTarget"), query.value(7)},
            {QStringLiteral("openUrl"), query.value(8)},
            {QStringLiteral("status"), query.value(9)},
            {QStringLiteral("createdAt"), query.value(10)},
            {QStringLiteral("completedAt"), query.value(11)},
            {QStringLiteral("viewedAt"), query.value(12)}};
}

QJsonObject taskObject(const QVariantMap &map) { return QJsonObject::fromVariantMap(map); }

} // namespace

class TaskStore::Private {
public:
    QSqlDatabase db;
    QString connectionName;
    QString dataDirectory;
    QString databasePath;
    QString themeId = QStringLiteral("default");
    QString wallpaperPath;
    QString userIconPath;
    bool ready = false;
};

TaskStore::TaskStore(QObject *parent) : QObject(parent), d(std::make_unique<Private>()) {
    // 与 Python 服务端保持同一数据目录，原生版可以无缝接管已有 SQLite 数据。
    const QString appData = qEnvironmentVariable("APPDATA");
    d->dataDirectory = appData.isEmpty()
                           ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                           : QDir(appData).filePath(QStringLiteral("AI Task Hub"));
    QDir().mkpath(d->dataDirectory);
    d->databasePath = QDir(d->dataDirectory).filePath(QStringLiteral("data.sqlite"));
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("AI Task Hub"), QStringLiteral("AI Task Hub"));
    const QString configuredTheme = settings.value(QStringLiteral("themeId"), QStringLiteral("default")).toString();
    if (kThemes.contains(configuredTheme)) d->themeId = configuredTheme;
    const QString configuredWallpaper = settings.value(QStringLiteral("wallpaperPath")).toString();
    if (QFileInfo::exists(configuredWallpaper)) d->wallpaperPath = configuredWallpaper;
    const QString configuredIcon = settings.value(QStringLiteral("userIconPath")).toString();
    if (QFileInfo::exists(configuredIcon)) d->userIconPath = configuredIcon;
    d->connectionName = QStringLiteral("ai-task-hub-native-%1").arg(reinterpret_cast<quintptr>(this));
    d->db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), d->connectionName);
    d->db.setDatabaseName(d->databasePath);
    if (!d->db.open()) {
        setError(QStringLiteral("无法打开本地数据库：%1").arg(d->db.lastError().text()));
        return;
    }

    QSqlQuery pragma(d->db);
    pragma.exec(QStringLiteral("PRAGMA foreign_keys = ON"));
    pragma.exec(QStringLiteral("PRAGMA journal_mode = WAL"));
    pragma.exec(QStringLiteral("PRAGMA synchronous = NORMAL"));
    pragma.exec(QStringLiteral("PRAGMA busy_timeout = 5000"));

    QSqlQuery query(d->db);
    const QString createTask = QStringLiteral(
        "CREATE TABLE IF NOT EXISTS task ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, source TEXT NOT NULL, external_id TEXT NOT NULL DEFAULT '',"
        "event_type TEXT NOT NULL, title TEXT, content_preview TEXT, project_path TEXT, open_target TEXT,"
        "open_url TEXT, status TEXT NOT NULL, created_at TEXT NOT NULL, completed_at TEXT, viewed_at TEXT,"
        "UNIQUE(source, external_id))");
    if (!query.exec(createTask)) {
        setError(QStringLiteral("初始化任务表失败：%1").arg(query.lastError().text()));
        return;
    }

    // 兼容 Python SQLite schema（external_task_id）和上一版 native schema（external_id）。
    QSet<QString> columns;
    QSqlQuery info(d->db);
    if (info.exec(QStringLiteral("PRAGMA table_info(task)"))) {
        while (info.next()) columns.insert(info.value(1).toString());
    }
    if (!columns.contains(QStringLiteral("external_id")) && columns.contains(QStringLiteral("external_task_id"))) {
        if (!query.exec(QStringLiteral("ALTER TABLE task RENAME COLUMN external_task_id TO external_id"))) {
            setError(QStringLiteral("升级任务 ID 字段失败：%1").arg(query.lastError().text()));
            return;
        }
        columns.remove(QStringLiteral("external_task_id"));
        columns.insert(QStringLiteral("external_id"));
    }
    const QList<QPair<QString, QString>> migrations{
        {QStringLiteral("content_preview"), QStringLiteral("TEXT")},
        {QStringLiteral("project_path"), QStringLiteral("TEXT")},
        {QStringLiteral("open_target"), QStringLiteral("TEXT")},
        {QStringLiteral("open_url"), QStringLiteral("TEXT")},
        {QStringLiteral("completed_at"), QStringLiteral("TEXT")},
        {QStringLiteral("viewed_at"), QStringLiteral("TEXT")}};
    for (const auto &migration : migrations) {
        if (!columns.contains(migration.first)) {
            if (!query.exec(QStringLiteral("ALTER TABLE task ADD COLUMN %1 %2")
                                .arg(migration.first, migration.second))) {
                setError(QStringLiteral("升级任务表失败：%1").arg(query.lastError().text()));
                return;
            }
            columns.insert(migration.first);
        }
    }
    if (columns.contains(QStringLiteral("preview")) && columns.contains(QStringLiteral("content_preview"))) {
        query.exec(QStringLiteral("UPDATE task SET content_preview = preview "
                                 "WHERE (content_preview IS NULL OR content_preview = '') AND preview IS NOT NULL"));
    }
    query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_task_status ON task(status)"));
    query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_task_created_at ON task(created_at)"));
    if (!query.exec(QStringLiteral(
            "CREATE TABLE IF NOT EXISTS task_event (id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "task_id INTEGER NOT NULL, event_type TEXT NOT NULL, raw_payload TEXT, created_at TEXT NOT NULL, "
            "FOREIGN KEY(task_id) REFERENCES task(id) ON DELETE CASCADE)"))) {
        setError(QStringLiteral("初始化事件表失败：%1").arg(query.lastError().text()));
        return;
    }
    query.exec(QStringLiteral("CREATE INDEX IF NOT EXISTS idx_task_event_task ON task_event(task_id, id)"));
    d->ready = true;
    clearError();
    refresh();
}

TaskStore::~TaskStore() {
    if (d && d->db.isOpen()) d->db.close();
    if (d) {
        const QString connectionName = d->connectionName;
        d->db = QSqlDatabase();
        QSqlDatabase::removeDatabase(connectionName);
    }
}

QVariantList TaskStore::tasks() const { return m_tasks; }
int TaskStore::totalTasks() const { return m_summary.value(QStringLiteral("total")).toInt(); }
int TaskStore::queueCount() const { return m_summary.value(QStringLiteral("queue")).toInt(); }
int TaskStore::historyCount() const { return m_summary.value(QStringLiteral("history")).toInt(); }
QString TaskStore::databasePath() const { return d->databasePath; }
QString TaskStore::dataDirectory() const { return d->dataDirectory; }
QString TaskStore::themeId() const { return d->themeId; }
QString TaskStore::wallpaperPath() const {
    return d->wallpaperPath.isEmpty() ? QString() : QUrl::fromLocalFile(d->wallpaperPath).toString();
}
QString TaskStore::userIconPath() const {
    return d->userIconPath.isEmpty() ? QString() : QUrl::fromLocalFile(d->userIconPath).toString();
}
bool TaskStore::isReady() const { return d->ready; }
QString TaskStore::lastError() const { return m_lastError; }
qint64 TaskStore::lastIngestedTaskId() const { return m_lastIngestedTaskId; }

void TaskStore::setError(const QString &message) {
    if (m_lastError == message) return;
    m_lastError = message;
    emit errorChanged();
}

void TaskStore::clearError() {
    if (m_lastError.isEmpty()) return;
    m_lastError.clear();
    emit errorChanged();
}

QJsonArray TaskStore::tasksJson(int limit, int offset) const {
    QJsonArray result;
    if (!d->ready) return result;
    QSqlQuery query(d->db);
    query.prepare(QStringLiteral(
        "SELECT id, source, external_id, event_type, title, content_preview, project_path, open_target, "
        "open_url, status, created_at, completed_at, viewed_at FROM task "
        "ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?"));
    query.addBindValue(std::max(1, std::min(limit, 500)));
    query.addBindValue(std::max(0, offset));
    if (!query.exec()) return result;
    while (query.next()) result.append(taskObject(taskMap(query)));
    return result;
}

QJsonArray TaskStore::tasksForStatusJson(const QString &status, int limit, int offset,
                                         bool *hasMore) const {
    if (hasMore) *hasMore = false;
    QJsonArray result;
    if (!d->ready || !contains(kStatuses, status)) return result;
    QSqlQuery query(d->db);
    query.prepare(QStringLiteral(
        "SELECT id, source, external_id, event_type, title, content_preview, project_path, open_target, "
        "open_url, status, created_at, completed_at, viewed_at FROM task WHERE status = ? "
        "ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?"));
    const int safeLimit = std::max(1, std::min(limit, 500));
    query.addBindValue(status);
    query.addBindValue(safeLimit + 1);
    query.addBindValue(std::max(0, offset));
    if (!query.exec()) return result;
    while (query.next()) result.append(taskObject(taskMap(query)));
    if (hasMore && result.size() > safeLimit) {
        *hasMore = true;
        result.removeLast();
    }
    return result;
}

QJsonArray TaskStore::tasksForViewJson(const QString &view, int limit, int offset,
                                       bool *hasMore) const {
    if (hasMore) *hasMore = false;
    QJsonArray result;
    if (!d->ready) return result;
    const QString filter = view == QStringLiteral("history")
                               ? QStringLiteral("('VIEWED','IGNORED')")
                               : QStringLiteral("('RUNNING','NEEDS_INPUT','COMPLETED_UNREAD','FAILED_UNREAD')");
    QSqlQuery query(d->db);
    query.prepare(QStringLiteral(
        "SELECT id, source, external_id, event_type, title, content_preview, project_path, open_target, "
        "open_url, status, created_at, completed_at, viewed_at FROM task WHERE status IN %1 "
        "ORDER BY created_at DESC, id DESC LIMIT ? OFFSET ?").arg(filter));
    const int safeLimit = std::max(1, std::min(limit, 500));
    query.addBindValue(safeLimit + 1);
    query.addBindValue(std::max(0, offset));
    if (!query.exec()) return result;
    while (query.next()) result.append(taskObject(taskMap(query)));
    if (hasMore && result.size() > safeLimit) {
        *hasMore = true;
        result.removeLast();
    }
    return result;
}

QJsonObject TaskStore::summaryJson() const { return m_summary; }

int TaskStore::eventCount() const {
    if (!d->ready) return 0;
    QSqlQuery query(d->db);
    return query.exec(QStringLiteral("SELECT COUNT(*) FROM task_event")) && query.next()
               ? query.value(0).toInt()
               : 0;
}

QJsonObject TaskStore::taskJson(qint64 id) const {
    if (!d->ready || id <= 0) return {};
    QSqlQuery query(d->db);
    query.prepare(QStringLiteral(
        "SELECT id, source, external_id, event_type, title, content_preview, project_path, open_target, "
        "open_url, status, created_at, completed_at, viewed_at FROM task WHERE id = ?"));
    query.addBindValue(id);
    return query.exec() && query.next() ? taskObject(taskMap(query)) : QJsonObject{};
}

QVariantMap TaskStore::taskVariant(qint64 id) const { return taskJson(id).toVariantMap(); }

QJsonArray TaskStore::eventsJson(qint64 id) const {
    QJsonArray events;
    if (!d->ready || id <= 0) return events;
    QSqlQuery query(d->db);
    query.prepare(QStringLiteral(
        "SELECT id, task_id, event_type, raw_payload, created_at FROM task_event "
        "WHERE task_id = ? ORDER BY id ASC"));
    query.addBindValue(id);
    if (!query.exec()) return events;
    while (query.next()) {
        const QJsonDocument payload = QJsonDocument::fromJson(query.value(3).toString().toUtf8());
        events.append(QJsonObject{{QStringLiteral("id"), query.value(0).toLongLong()},
                                  {QStringLiteral("taskId"), query.value(1).toLongLong()},
                                  {QStringLiteral("eventType"), query.value(2).toString()},
                                  {QStringLiteral("occurredAt"), query.value(4).toString()},
                                  {QStringLiteral("payload"), payload.isObject() ? QJsonValue(payload.object()) : QJsonValue(QJsonObject{})}});
    }
    return events;
}

QVariantList TaskStore::eventVariants(qint64 id) const {
    QVariantList result;
    for (const auto &event : eventsJson(id)) result.append(event.toObject().toVariantMap());
    return result;
}

QJsonObject TaskStore::aiReplyJson(qint64 id) const {
    QJsonObject result{{QStringLiteral("taskId"), id}, {QStringLiteral("content"), QJsonValue()},
                       {QStringLiteral("error"), QStringLiteral("暂无可用的答复记录")}};
    for (const auto &event : eventsJson(id)) {
        const QJsonObject payload = event.toObject().value(QStringLiteral("payload")).toObject();
        const QString reply = payload.value(QStringLiteral("replyText")).toString().trimmed();
        if (!reply.isEmpty()) {
            result[QStringLiteral("content")] = reply;
            result[QStringLiteral("error")] = QJsonValue();
        }
    }
    return result;
}

qint64 TaskStore::ingestEvent(const QJsonObject &event) {
    if (!d->ready) {
        setError(QStringLiteral("本地数据库尚未就绪"));
        return 0;
    }
    if (QJsonDocument(event).toJson(QJsonDocument::Compact).size() > kMaxBodyBytes) {
        setError(QStringLiteral("事件内容超过 1 MiB 限制"));
        return 0;
    }
    const QString source = event.value(QStringLiteral("source")).toString().trimmed();
    const QString eventType = event.value(QStringLiteral("eventType")).toString().trimmed();
    if (!contains(kSources, source) || !contains(kEventTypes, eventType)) {
        setError(QStringLiteral("source 或 eventType 不在 AgentEvent 协议范围内"));
        return 0;
    }
    const QString externalId = bounded(event.value(QStringLiteral("externalTaskId")), kMaxExternalId);
    const QString titleInput = bounded(event.value(QStringLiteral("title")), kMaxTitle);
    const QString preview = bounded(event.value(QStringLiteral("contentPreview")), kMaxPreview);
    const QString reply = bounded(event.value(QStringLiteral("replyText")), kMaxReply);
    const QString projectPath = bounded(event.value(QStringLiteral("projectPath")), kMaxPath);
    const QString openUrl = bounded(event.value(QStringLiteral("openUrl")), kMaxUrl);
    const QString openTarget = event.value(QStringLiteral("openTarget")).toString().trimmed();
    if (!openTarget.isEmpty() && !QStringList{QStringLiteral("browser"), QStringLiteral("terminal"), QStringLiteral("none")}.contains(openTarget)) {
        setError(QStringLiteral("openTarget 只能是 browser、terminal 或 none"));
        return 0;
    }
    QJsonObject normalized = event;
    if (!reply.isEmpty()) normalized.insert(QStringLiteral("replyText"), reply);
    const QString title = titleInput.isEmpty() ? deriveTitle(preview) : titleInput;
    normalized.insert(QStringLiteral("title"), title);
    const QString createdAtValue = event.value(QStringLiteral("createdAt")).toString();
    const QString createdAt = QDateTime::fromString(createdAtValue, Qt::ISODate).isValid() ? createdAtValue : nowIso();
    const QString status = statusFor(eventType);
    const QString rawPayload = QString::fromUtf8(QJsonDocument(normalized).toJson(QJsonDocument::Compact));
    if (!d->db.transaction()) {
        setError(QStringLiteral("开启数据库事务失败：%1").arg(d->db.lastError().text()));
        return 0;
    }

    qint64 id = 0;
    QSqlQuery find(d->db);
    find.prepare(QStringLiteral("SELECT id FROM task WHERE source = ? AND (external_id = ? OR (external_id IS NULL AND ? = '')) LIMIT 1"));
    find.addBindValue(source);
    find.addBindValue(externalId);
    find.addBindValue(externalId);
    if (find.exec() && find.next()) id = find.value(0).toLongLong();

    QSqlQuery query(d->db);
    if (id == 0) {
        query.prepare(QStringLiteral(
            "INSERT INTO task(source, external_id, event_type, title, content_preview, project_path, open_target, open_url, status, created_at, completed_at, viewed_at) "
            "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)"));
        query.addBindValue(source); query.addBindValue(externalId); query.addBindValue(eventType);
        query.addBindValue(title); query.addBindValue(preview); query.addBindValue(projectPath);
        query.addBindValue(openTarget); query.addBindValue(openUrl); query.addBindValue(status);
        query.addBindValue(createdAt);
        query.addBindValue((eventType == QStringLiteral("TASK_COMPLETED") || eventType == QStringLiteral("TASK_FAILED")) ? createdAt : QVariant());
        query.addBindValue(eventType == QStringLiteral("TASK_VIEWED") ? QVariant(createdAt) : QVariant());
        if (!query.exec()) {
            d->db.rollback();
            setError(QStringLiteral("写入任务失败：%1").arg(query.lastError().text()));
            return 0;
        }
        id = query.lastInsertId().toLongLong();
    } else {
        query.prepare(QStringLiteral(
            "UPDATE task SET event_type=?, title=CASE WHEN ? <> '' THEN ? ELSE title END, "
            "content_preview=CASE WHEN ? <> '' THEN ? ELSE content_preview END, "
            "project_path=CASE WHEN ? <> '' THEN ? ELSE project_path END, "
            "open_target=CASE WHEN ? <> '' THEN ? ELSE open_target END, "
            "open_url=CASE WHEN ? <> '' THEN ? ELSE open_url END, status=?, "
            "completed_at=CASE WHEN ? IN ('COMPLETED_UNREAD','FAILED_UNREAD') THEN ? ELSE completed_at END, "
            "viewed_at=CASE WHEN ? = 'VIEWED' THEN ? WHEN ? IN ('RUNNING','NEEDS_INPUT','COMPLETED_UNREAD','FAILED_UNREAD') THEN NULL ELSE viewed_at END WHERE id=?"));
        query.addBindValue(eventType);
        query.addBindValue(titleInput); query.addBindValue(titleInput);
        query.addBindValue(preview); query.addBindValue(preview);
        query.addBindValue(projectPath); query.addBindValue(projectPath);
        query.addBindValue(openTarget); query.addBindValue(openTarget);
        query.addBindValue(openUrl); query.addBindValue(openUrl);
        query.addBindValue(status);
        query.addBindValue(status); query.addBindValue(createdAt);
        query.addBindValue(status); query.addBindValue(createdAt); query.addBindValue(status); query.addBindValue(id);
        if (!query.exec()) {
            d->db.rollback();
            setError(QStringLiteral("更新任务失败：%1").arg(query.lastError().text()));
            return 0;
        }
    }

    QSqlQuery eventQuery(d->db);
    eventQuery.prepare(QStringLiteral("INSERT INTO task_event(task_id, event_type, raw_payload, created_at) VALUES(?,?,?,?)"));
    eventQuery.addBindValue(id); eventQuery.addBindValue(eventType); eventQuery.addBindValue(rawPayload); eventQuery.addBindValue(createdAt);
    if (!eventQuery.exec() || !d->db.commit()) {
        d->db.rollback();
        setError(QStringLiteral("写入事件流水失败：%1").arg(eventQuery.lastError().text()));
        return 0;
    }
    clearError();
    m_lastIngestedTaskId = id;
    refresh();
    emit taskReceived(id, title, source, status);
    return id;
}

bool TaskStore::ingest(const QJsonObject &event) { return ingestEvent(event) > 0; }

void TaskStore::refresh() {
    if (!d->ready) return;
    QVariantList next;
    QJsonArray nextJson;
    QSqlQuery query(d->db);
    query.prepare(QStringLiteral(
        "SELECT id, source, external_id, event_type, title, content_preview, project_path, open_target, open_url, status, created_at, completed_at, viewed_at "
        "FROM task ORDER BY created_at DESC, id DESC LIMIT ?"));
    query.addBindValue(kRefreshLimit);
    if (query.exec()) {
        while (query.next()) {
            const QVariantMap row = taskMap(query);
            next.append(row);
            nextJson.append(taskObject(row));
        }
    }
    QJsonObject summary{{QStringLiteral("total"), 0}, {QStringLiteral("queue"), 0}, {QStringLiteral("history"), 0}};
    for (const QString &status : kStatuses) summary.insert(status, 0);
    QSqlQuery count(d->db);
    if (count.exec(QStringLiteral("SELECT status, COUNT(*) FROM task GROUP BY status"))) {
        while (count.next()) {
            const QString status = count.value(0).toString();
            const int number = count.value(1).toInt();
            summary.insert(status, number);
            summary[QStringLiteral("total")] = summary.value(QStringLiteral("total")).toInt() + number;
            if (status == QStringLiteral("VIEWED") || status == QStringLiteral("IGNORED"))
                summary[QStringLiteral("history")] = summary.value(QStringLiteral("history")).toInt() + number;
            else
                summary[QStringLiteral("queue")] = summary.value(QStringLiteral("queue")).toInt() + number;
        }
    }
    m_tasks = next;
    m_tasksJson = nextJson;
    m_summary = summary;
    emit tasksChanged();
    emit summaryChanged();
}

bool TaskStore::setStatus(qint64 id, const QString &status) {
    if (!d->ready || (status != QStringLiteral("VIEWED") && status != QStringLiteral("IGNORED"))) return false;
    const QString eventType = status == QStringLiteral("VIEWED") ? QStringLiteral("TASK_VIEWED") : QStringLiteral("TASK_IGNORED");
    const QString timestamp = nowIso();
    if (!d->db.transaction()) return false;
    QSqlQuery query(d->db);
    query.prepare(QStringLiteral("UPDATE task SET status=?, event_type=?, viewed_at=? WHERE id=?"));
    query.addBindValue(status); query.addBindValue(eventType);
    query.addBindValue(status == QStringLiteral("VIEWED") ? QVariant(timestamp) : QVariant());
    query.addBindValue(id);
    if (!query.exec() || query.numRowsAffected() == 0) { d->db.rollback(); return false; }
    QSqlQuery event(d->db);
    event.prepare(QStringLiteral("INSERT INTO task_event(task_id,event_type,raw_payload,created_at) VALUES(?,?,?,?)"));
    event.addBindValue(id); event.addBindValue(eventType); event.addBindValue(QStringLiteral("{\"eventType\":\"%1\"}").arg(eventType)); event.addBindValue(timestamp);
    if (!event.exec() || !d->db.commit()) { d->db.rollback(); return false; }
    refresh();
    return true;
}

bool TaskStore::remove(qint64 id) {
    if (!d->ready || !d->db.transaction()) return false;
    QSqlQuery query(d->db); query.prepare(QStringLiteral("DELETE FROM task WHERE id=?")); query.addBindValue(id);
    const bool ok = query.exec() && query.numRowsAffected() > 0 && d->db.commit();
    if (!ok) d->db.rollback();
    if (ok) refresh();
    return ok;
}

int TaskStore::clear(const QString &scope) {
    if (!d->ready || !d->db.transaction()) return 0;
    QString sql = QStringLiteral("DELETE FROM task");
    if (scope == QStringLiteral("completed")) sql += QStringLiteral(" WHERE status = 'COMPLETED_UNREAD'");
    else if (scope == QStringLiteral("queue")) sql += QStringLiteral(" WHERE status IN ('RUNNING','NEEDS_INPUT','COMPLETED_UNREAD','FAILED_UNREAD')");
    else if (scope == QStringLiteral("history")) sql += QStringLiteral(" WHERE status IN ('VIEWED','IGNORED')");
    QSqlQuery query(d->db);
    if (!query.exec(sql) || !d->db.commit()) { d->db.rollback(); return 0; }
    const int count = query.numRowsAffected();
    refresh();
    return count;
}

int TaskStore::markAllViewed() {
    if (!d->ready || !d->db.transaction()) return 0;
    const QString timestamp = nowIso();
    QSqlQuery ids(d->db);
    if (!ids.exec(QStringLiteral("SELECT id FROM task WHERE status IN ('COMPLETED_UNREAD','FAILED_UNREAD') ORDER BY id"))) { d->db.rollback(); return 0; }
    QList<qint64> taskIds;
    while (ids.next()) taskIds.append(ids.value(0).toLongLong());
    if (taskIds.isEmpty()) { d->db.commit(); return 0; }
    QSqlQuery update(d->db);
    if (!update.exec(QStringLiteral("UPDATE task SET status='VIEWED', event_type='TASK_VIEWED', viewed_at='%1' WHERE status IN ('COMPLETED_UNREAD','FAILED_UNREAD')").arg(timestamp))) { d->db.rollback(); return 0; }
    QSqlQuery event(d->db);
    event.prepare(QStringLiteral("INSERT INTO task_event(task_id,event_type,raw_payload,created_at) VALUES(?,?,?,?)"));
    for (const qint64 id : taskIds) {
        event.addBindValue(id); event.addBindValue(QStringLiteral("TASK_VIEWED")); event.addBindValue(QStringLiteral("{\"eventType\":\"TASK_VIEWED\"}")); event.addBindValue(timestamp);
        if (!event.exec()) { d->db.rollback(); return 0; }
        event.finish();
    }
    if (!d->db.commit()) { d->db.rollback(); return 0; }
    refresh();
    return taskIds.size();
}

bool TaskStore::openTask(qint64 id) {
    const QJsonObject task = taskJson(id);
    if (task.isEmpty()) return false;
    const QString target = task.value(QStringLiteral("openTarget")).toString();
    const QString url = task.value(QStringLiteral("openUrl")).toString();
    const QString path = task.value(QStringLiteral("projectPath")).toString();
    if ((target == QStringLiteral("browser") || (target.isEmpty() && !url.isEmpty())) && !url.isEmpty())
        return QDesktopServices::openUrl(QUrl(url));
    if ((target == QStringLiteral("terminal") || !path.isEmpty()) && !path.isEmpty()) {
        if (QProcess::startDetached(QStringLiteral("wt.exe"), {QStringLiteral("-d"), path})) return true;
        return QProcess::startDetached(QStringLiteral("explorer.exe"), {path});
    }
    return !url.isEmpty() && QDesktopServices::openUrl(QUrl(url));
}

bool TaskStore::openDataDirectory() { return QDesktopServices::openUrl(QUrl::fromLocalFile(d->dataDirectory)); }

bool TaskStore::setTheme(const QString &themeId) {
    if (!kThemes.contains(themeId)) return false;
    if (d->themeId == themeId) return true;
    d->themeId = themeId;
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("AI Task Hub"), QStringLiteral("AI Task Hub"));
    settings.setValue(QStringLiteral("themeId"), themeId);
    settings.sync();
    emit appearanceChanged();
    return true;
}

bool TaskStore::setUserIconPreset(const QString &presetId) {
    if (!kThemes.contains(presetId)) return false;
    const QString source = bundledPresetPath(presetId);
    if (source.isEmpty()) {
        setError(QStringLiteral("找不到内置头像：%1").arg(presetId));
        return false;
    }

    const QString destination = QDir(d->dataDirectory).filePath(
        QStringLiteral("icon-preset-%1.png").arg(presetId));
    if (d->userIconPath != destination) {
        const QString dataRoot = QDir(d->dataDirectory).absolutePath() + QDir::separator();
        if (d->userIconPath.startsWith(dataRoot, Qt::CaseInsensitive)) QFile::remove(d->userIconPath);
        QFile::remove(destination);
        if (!QFile::copy(source, destination)) {
            setError(QStringLiteral("复制内置头像失败，请检查文件权限"));
            return false;
        }
        d->userIconPath = destination;
    }

    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("AI Task Hub"), QStringLiteral("AI Task Hub"));
    settings.setValue(QStringLiteral("userIconPath"), d->userIconPath);
    settings.sync();
    clearError();
    emit appearanceChanged();
    return true;
}

QString TaskStore::pickWallpaper() {
    const QString selected = QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("选择壁纸"), QString(),
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.webp *.bmp)"));
    if (selected.isEmpty()) return wallpaperPath();
    const QString suffix = QFileInfo(selected).suffix().toLower();
    const QString destination = QDir(d->dataDirectory).filePath(QStringLiteral("wallpaper.%1").arg(suffix));
    if (!d->wallpaperPath.isEmpty() && d->wallpaperPath != destination) QFile::remove(d->wallpaperPath);
    QFile::remove(destination);
    if (!QFile::copy(selected, destination)) {
        setError(QStringLiteral("复制壁纸失败，请检查文件权限"));
        return wallpaperPath();
    }
    d->wallpaperPath = destination;
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("AI Task Hub"), QStringLiteral("AI Task Hub"));
    settings.setValue(QStringLiteral("wallpaperPath"), destination);
    settings.sync();
    clearError();
    emit appearanceChanged();
    return wallpaperPath();
}

void TaskStore::clearWallpaper() {
    if (!d->wallpaperPath.isEmpty()) QFile::remove(d->wallpaperPath);
    d->wallpaperPath.clear();
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("AI Task Hub"), QStringLiteral("AI Task Hub"));
    settings.remove(QStringLiteral("wallpaperPath"));
    settings.sync();
    emit appearanceChanged();
}

QString TaskStore::pickUserIcon() {
    const QString selected = QFileDialog::getOpenFileName(
        nullptr, QStringLiteral("选择应用头像"), QString(),
        QStringLiteral("图片 (*.png *.jpg *.jpeg *.webp *.bmp)"));
    if (selected.isEmpty()) return userIconPath();
    const QString suffix = QFileInfo(selected).suffix().toLower();
    const QString destination = QDir(d->dataDirectory).filePath(QStringLiteral("icon.%1").arg(suffix));
    const QString dataRoot = QDir(d->dataDirectory).absolutePath() + QDir::separator();
    if (!d->userIconPath.isEmpty() && d->userIconPath != destination &&
        d->userIconPath.startsWith(dataRoot, Qt::CaseInsensitive))
        QFile::remove(d->userIconPath);
    QFile::remove(destination);
    if (!QFile::copy(selected, destination)) {
        setError(QStringLiteral("复制头像失败，请检查文件权限"));
        return userIconPath();
    }
    d->userIconPath = destination;
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("AI Task Hub"), QStringLiteral("AI Task Hub"));
    settings.setValue(QStringLiteral("userIconPath"), destination);
    settings.sync();
    clearError();
    emit appearanceChanged();
    return userIconPath();
}

void TaskStore::clearUserIcon() {
    const QString dataRoot = QDir(d->dataDirectory).absolutePath() + QDir::separator();
    if (d->userIconPath.startsWith(dataRoot, Qt::CaseInsensitive)) QFile::remove(d->userIconPath);
    d->userIconPath.clear();
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QStringLiteral("AI Task Hub"), QStringLiteral("AI Task Hub"));
    settings.remove(QStringLiteral("userIconPath"));
    settings.sync();
    emit appearanceChanged();
}

void TaskStore::showWindow() {
    const auto windows = QGuiApplication::allWindows();
    if (!windows.isEmpty()) {
        windows.first()->show();
        windows.first()->raise();
        windows.first()->requestActivate();
    }
}

void TaskStore::hideWindow() {
    const auto windows = QGuiApplication::allWindows();
    if (!windows.isEmpty()) windows.first()->hide();
}

void TaskStore::quitApplication() { QCoreApplication::quit(); }
