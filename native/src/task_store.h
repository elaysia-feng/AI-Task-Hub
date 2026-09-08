#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QVariantList>

#include <memory>

/**
 * 原生桌面端的任务存储与状态机。
 *
 * 这里直接复用 AgentEvent 的 camelCase JSON 契约，SQLite 只负责本地持久化。
 * 所有写操作都在一个短事务中完成，事件流水和任务状态不会出现半套提交。
 */
class TaskStore final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList tasks READ tasks NOTIFY tasksChanged)
    Q_PROPERTY(int totalTasks READ totalTasks NOTIFY summaryChanged)
    Q_PROPERTY(int queueCount READ queueCount NOTIFY summaryChanged)
    Q_PROPERTY(int historyCount READ historyCount NOTIFY summaryChanged)
    Q_PROPERTY(QString databasePath READ databasePath CONSTANT)
    Q_PROPERTY(QString dataDirectory READ dataDirectory CONSTANT)
    Q_PROPERTY(QString themeId READ themeId NOTIFY appearanceChanged)
    Q_PROPERTY(QString wallpaperPath READ wallpaperPath NOTIFY appearanceChanged)
    Q_PROPERTY(QString userIconPath READ userIconPath NOTIFY appearanceChanged)
    Q_PROPERTY(bool ready READ isReady CONSTANT)
    Q_PROPERTY(QString lastError READ lastError NOTIFY errorChanged)

public:
    explicit TaskStore(QObject *parent = nullptr);
    ~TaskStore() override;

    QVariantList tasks() const;
    int totalTasks() const;
    int queueCount() const;
    int historyCount() const;
    QString databasePath() const;
    QString dataDirectory() const;
    QString themeId() const;
    QString wallpaperPath() const;
    QString userIconPath() const;
    bool isReady() const;
    QString lastError() const;

    QJsonArray tasksJson(int limit = 200, int offset = 0) const;
    QJsonArray tasksForStatusJson(const QString &status, int limit = 40, int offset = 0,
                                  bool *hasMore = nullptr) const;
    QJsonArray tasksForViewJson(const QString &view, int limit = 200, int offset = 0,
                                bool *hasMore = nullptr) const;
    QJsonObject summaryJson() const;
    int eventCount() const;
    QJsonObject taskJson(qint64 id) const;
    QJsonArray eventsJson(qint64 id) const;
    QJsonObject aiReplyJson(qint64 id) const;

    /** 写入一条事件，成功后返回任务 ID；失败返回 0 并设置 lastError。 */
    qint64 ingestEvent(const QJsonObject &event);
    bool ingest(const QJsonObject &event);
    qint64 lastIngestedTaskId() const;

    Q_INVOKABLE QVariantMap taskVariant(qint64 id) const;
    Q_INVOKABLE QVariantList eventVariants(qint64 id) const;
    Q_INVOKABLE bool setStatus(qint64 id, const QString &status);
    Q_INVOKABLE bool remove(qint64 id);
    Q_INVOKABLE int clear(const QString &scope = QStringLiteral("all"));
    Q_INVOKABLE int markAllViewed();
    Q_INVOKABLE bool openTask(qint64 id);
    Q_INVOKABLE bool openDataDirectory();
    Q_INVOKABLE bool setTheme(const QString &themeId);
    Q_INVOKABLE bool setUserIconPreset(const QString &presetId);
    Q_INVOKABLE QString pickWallpaper();
    Q_INVOKABLE void clearWallpaper();
    Q_INVOKABLE QString pickUserIcon();
    Q_INVOKABLE void clearUserIcon();
    Q_INVOKABLE void showWindow();
    Q_INVOKABLE void hideWindow();
    Q_INVOKABLE void quitApplication();

signals:
    void tasksChanged();
    void summaryChanged();
    void errorChanged();
    void appearanceChanged();
    void taskReceived(qint64 id, const QString &title, const QString &source,
                      const QString &status);

private:
    class Private;
    std::unique_ptr<Private> d;
    QVariantList m_tasks;
    QJsonArray m_tasksJson;
    QJsonObject m_summary;
    qint64 m_lastIngestedTaskId = 0;
    QString m_lastError;

    void refresh();
    void setError(const QString &message);
    void clearError();
};
