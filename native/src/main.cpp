#include <QApplication>
#include <QDateTime>
#include <QCursor>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QHttpHeaders>
#include <QHttpServer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QHash>
#include <QQueue>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QRegion>
#include <QScreen>
#include <QSGRendererInterface>
#include <QSystemTrayIcon>
#include <QTcpServer>
#include <QTimer>
#include <QUrlQuery>
#include <QWindow>

#include "task_store.h"

namespace {

QHttpServerResponse jsonResponse(
    const QJsonObject &object,
    QHttpServerResponse::StatusCode status = QHttpServerResponse::StatusCode::Ok) {
    QHttpServerResponse response(object, status);
    QHttpHeaders headers = response.headers();
    headers.replaceOrAppend(QHttpHeaders::WellKnownHeader::AccessControlAllowOrigin, "*");
    headers.replaceOrAppend(QHttpHeaders::WellKnownHeader::AccessControlAllowHeaders,
                            "Content-Type, Authorization");
    headers.replaceOrAppend(QHttpHeaders::WellKnownHeader::AccessControlAllowMethods,
                            "GET, POST, DELETE, OPTIONS");
    headers.replaceOrAppend(QHttpHeaders::WellKnownHeader::CacheControl, "no-store");
    response.setHeaders(std::move(headers));
    return response;
}

QHttpServerResponse optionsResponse() {
    return jsonResponse(QJsonObject{}, QHttpServerResponse::StatusCode::NoContent);
}

int queryInt(const QHttpServerRequest &request, const QString &name, int fallback,
             int minimum, int maximum) {
    bool ok = false;
    const int value = request.query().queryItemValue(name).toInt(&ok);
    return ok ? qBound(minimum, value, maximum) : fallback;
}

QIcon makeTrayIcon() {
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor("#6d7cff"));
    painter.setPen(Qt::NoPen);
    painter.drawEllipse(4, 4, 56, 56);
    painter.setBrush(QColor("#ffffff"));
    painter.drawRoundedRect(18, 17, 28, 30, 7, 7);
    painter.setPen(QPen(QColor("#6d7cff"), 3));
    painter.drawLine(24, 27, 40, 27);
    painter.drawLine(24, 34, 36, 34);
    painter.drawLine(24, 41, 32, 41);
    return QIcon(pixmap);
}

QString resourceDirectory() {
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates{
        QDir(appDir).filePath(QStringLiteral("resources")),
        QDir(appDir).filePath(QStringLiteral("../desktop/resources")),
        QDir(appDir).filePath(QStringLiteral("../../desktop/resources"))};
    for (const QString &candidate : candidates) {
        const QString clean = QDir::cleanPath(candidate);
        if (QFileInfo::exists(QDir(clean).filePath(QStringLiteral("presets/default.png")))) return clean;
    }
    return QDir(appDir).filePath(QStringLiteral("resources"));
}

QIcon bundledAppIcon() {
    const QString resources = resourceDirectory();
    for (const QString &name : {QStringLiteral("icon.ico"), QStringLiteral("icon.png"),
                                 QStringLiteral("tray.png")}) {
        const QIcon icon(QDir(resources).filePath(name));
        if (!icon.isNull()) return icon;
    }
    return makeTrayIcon();
}

class OrbGestureFilter final : public QObject {
public:
    OrbGestureFilter(QWindow *window, QObject *root, QObject *parent)
        : QObject(parent), m_window(window), m_root(root) {}

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (watched != m_window || !m_root->property("orbMode").toBool()) {
            m_tracking = false;
            return false;
        }

        if (event->type() == QEvent::Enter) {
            if (!m_root->property("orbHoverExpanded").toBool()) expandOrbPreview();
            return false;
        }

        if (event->type() == QEvent::Leave) {
            if (m_root->property("orbHoverExpanded").toBool()) schedulePreviewCollapse();
            return false;
        }

        if (event->type() == QEvent::MouseButtonPress) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) {
                // 悬停面板里的按钮不应被误判为小球点击；只有球面保留拖动/打开手势。
                if (m_root->property("orbHoverExpanded").toBool() &&
                    !QRect(orbPosition(), QSize(52, 52)).contains(mouse->globalPosition().toPoint())) {
                    return false;
                }
                m_tracking = true;
                m_dragged = false;
                m_pressGlobal = mouse->globalPosition().toPoint();
                m_pressOrbPosition = orbPosition();
            }
            return false;
        }

        if (event->type() == QEvent::MouseMove && m_tracking) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (!(mouse->buttons() & Qt::LeftButton)) {
                // 某些 Windows 输入注入路径可能只投递 MouseMove；不能让上一轮按下状态
                // 残留，否则下一次移动会被误算成拖拽。
                m_tracking = false;
                m_dragged = false;
                return false;
            }

            const QPoint delta = mouse->globalPosition().toPoint() - m_pressGlobal;
            if (!m_dragged && delta.manhattanLength() >= 3) m_dragged = true;
            if (m_dragged) {
                if (m_root->property("orbHoverExpanded").toBool()) {
                    collapseOrbPreview(m_pressOrbPosition + delta);
                }
                m_window->setPosition(m_pressOrbPosition + delta);
            }
            return false;
        }

        if (event->type() == QEvent::MouseButtonRelease) {
            const auto *mouse = static_cast<QMouseEvent *>(event);
            if (m_tracking && mouse->button() == Qt::LeftButton) {
                const bool openMainWindow = !m_dragged;
                m_tracking = false;
                if (openMainWindow) openMainWindowOnOppositeSide();
            }
        }
        return false;
    }

private:
    QPoint orbPosition() const {
        constexpr int orbSize = 52;
        if (!m_root->property("orbHoverExpanded").toBool()) return m_window->position();

        const bool panelToLeft = m_root->property("orbPanelToLeft").toBool();
        const QRect bounds = m_window->geometry();
        const int orbX = panelToLeft ? bounds.left() + bounds.width() - orbSize - 8 : bounds.left() + 8;
        const int orbY = bounds.top() + (bounds.height() - orbSize) / 2;
        return QPoint(orbX, orbY);
    }

    void expandOrbPreview() {
        constexpr int orbSize = 52;
        constexpr int previewWidth = 340;
        constexpr int previewHeight = 380;
        constexpr int screenMargin = 14;
        const QScreen *screen = m_window->screen();
        if (!screen) return;

        const QRect available = screen->availableGeometry();
        const QPoint orbTopLeft = m_window->position();
        const QPoint orbCenter = orbTopLeft + QPoint(orbSize / 2, orbSize / 2);
        const bool panelToLeft = orbCenter.x() >= available.center().x();
        const int orbOffsetX = panelToLeft ? previewWidth - orbSize - 8 : 8;
        const int desiredX = orbTopLeft.x() - orbOffsetX;
        const int minimumX = available.left() + screenMargin;
        const int maximumX = qMax(minimumX, available.right() - previewWidth + 1 - screenMargin);
        const int targetX = qBound(minimumX, desiredX, maximumX);
        const int minimumY = available.top() + screenMargin;
        const int maximumY = qMax(minimumY, available.bottom() - previewHeight + 1 - screenMargin);
        const int targetY = qBound(minimumY, orbCenter.y() - previewHeight / 2, maximumY);

        m_root->setProperty("orbPanelToLeft", panelToLeft);
        m_root->setProperty("orbHoverExpanded", true);
        m_window->setMask(QRegion());
        m_window->setGeometry(QRect(QPoint(targetX, targetY), QSize(previewWidth, previewHeight)));
        m_window->raise();
    }

    void collapseOrbPreview(const QPoint &targetPosition) {
        constexpr int orbSize = 52;
        m_root->setProperty("orbHoverExpanded", false);
        m_window->setGeometry(QRect(targetPosition, QSize(orbSize, orbSize)));
        m_window->setMask(QRegion(QRect(QPoint(0, 0), QSize(orbSize, orbSize)), QRegion::Ellipse));
    }

    void schedulePreviewCollapse() {
        QTimer::singleShot(180, m_window, [this] {
            if (!m_window || !m_root->property("orbMode").toBool() ||
                !m_root->property("orbHoverExpanded").toBool())
                return;
            if (!m_window->geometry().contains(QCursor::pos())) collapseOrbPreview(orbPosition());
        });
    }

    void openMainWindowOnOppositeSide() {
        constexpr QSize mainWindowSize(1020, 660);
        constexpr int screenMargin = 24;
        const QRect available = m_window->screen()->availableGeometry();
        const QPoint orbCenter = m_window->geometry().center();
        const bool orbOnLeft = orbCenter.x() < available.center().x();
        const int targetX = orbOnLeft
            ? available.right() - mainWindowSize.width() + 1 - screenMargin
            : available.left() + screenMargin;
        const int minimumY = available.top() + screenMargin;
        const int maximumY = qMax(minimumY,
                                  available.bottom() - mainWindowSize.height() + 1 - screenMargin);
        const int targetY = qBound(minimumY, orbCenter.y() - mainWindowSize.height() / 2, maximumY);
        const QPoint targetPosition(targetX, targetY);

        // 先异步恢复正常尺寸，再在同一屏幕的另一侧摆放主窗口，避免与悬浮球重叠。
        QMetaObject::invokeMethod(m_root, "leaveOrb", Qt::QueuedConnection);
        QTimer::singleShot(40, m_window, [window = m_window, targetPosition] {
            window->setPosition(targetPosition);
        });
    }

    QWindow *m_window;
    QObject *m_root;
    QPoint m_pressGlobal;
    QPoint m_pressOrbPosition;
    bool m_tracking = false;
    bool m_dragged = false;
};

} // namespace

int main(int argc, char *argv[]) {
    // 发行版不显示 Qt Quick 的场景图调试边界（例如黄色 clip 边框）。
    qunsetenv("QSG_VISUALIZE");
    // 低内存模式：避免 Qt Quick 启用 NVIDIA D3D 编译器（单独会映射约 180 MB）。
    // 这个应用以列表、图片和轻量动效为主，软件场景图足够流畅且显著降低常驻内存。
    qputenv("QSG_RHI_BACKEND", QByteArrayLiteral("software"));
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("AI Task Hub"));
    app.setApplicationVersion(QStringLiteral("0.2.0-native"));
    app.setOrganizationName(QStringLiteral("AI Task Hub"));
    app.setQuitOnLastWindowClosed(false);
    app.setWindowIcon(bundledAppIcon());
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    TaskStore taskStore;
    QHttpServer server;
    QHash<QString, QQueue<qint64>> eventRateBuckets;

    server.route("/api/health", [&taskStore] {
        return jsonResponse(QJsonObject{{QStringLiteral("status"), taskStore.isReady() ? "ok" : "degraded"},
                                        {QStringLiteral("service"), "AI Task Hub Native"},
                                        {QStringLiteral("version"), "0.2.0-native"}});
    });
    server.route("/api/status", [&taskStore] {
        const QJsonObject summary = taskStore.summaryJson();
        return jsonResponse(QJsonObject{{QStringLiteral("status"), taskStore.isReady() ? "ok" : "degraded"},
                                        {QStringLiteral("version"), "0.2.0-native"},
                                        {QStringLiteral("runtime"), "C++/Qt 6"},
                                        {QStringLiteral("db"), QJsonObject{{QStringLiteral("ok"), taskStore.isReady()},
                                                                              {QStringLiteral("backend"), "sqlite"},
                                                                              {QStringLiteral("database"), taskStore.databasePath()}}},
                                        {QStringLiteral("tasks"), summary.value(QStringLiteral("total"))},
                                        {QStringLiteral("events"), taskStore.eventCount()},
                                        {QStringLiteral("logFile"), QJsonValue()}});
    });
    server.route("/api/tasks", QHttpServerRequest::Method::Get,
                 [&taskStore](const QHttpServerRequest &request) {
        if (request.method() == QHttpServerRequest::Method::Options) return optionsResponse();
        const QString status = request.query().queryItemValue(QStringLiteral("status"));
        const int limit = queryInt(request, QStringLiteral("limit"), 200, 1, 500);
        const int offset = queryInt(request, QStringLiteral("offset"), 0, 0, 1'000'000);
        bool hasMore = false;
        QJsonArray tasks;
        if (!status.isEmpty()) {
            if (!QStringList{QStringLiteral("RUNNING"), QStringLiteral("NEEDS_INPUT"),
                             QStringLiteral("COMPLETED_UNREAD"), QStringLiteral("FAILED_UNREAD"),
                             QStringLiteral("VIEWED"), QStringLiteral("IGNORED")}.contains(status))
                return jsonResponse(QJsonObject{{QStringLiteral("detail"), "unknown task status"}},
                                    QHttpServerResponse::StatusCode::UnprocessableEntity);
            tasks = taskStore.tasksForStatusJson(status, limit, offset, &hasMore);
        }
        else {
            const QString view = request.query().queryItemValue(QStringLiteral("view"));
            if (view == QStringLiteral("queue") || view == QStringLiteral("history"))
                tasks = taskStore.tasksForViewJson(view, limit, offset, &hasMore);
            else
                tasks = taskStore.tasksJson(limit, offset);
        }
        return jsonResponse(QJsonObject{{QStringLiteral("tasks"), tasks}, {QStringLiteral("hasMore"), hasMore}});
    });
    server.route("/api/tasks/summary", [&taskStore] {
        return jsonResponse(QJsonObject{{QStringLiteral("counts"), taskStore.summaryJson()}});
    });
    server.route("/api/tasks/snapshot", [&taskStore](const QHttpServerRequest &request) {
        if (request.method() == QHttpServerRequest::Method::Options) return optionsResponse();
        const int limit = queryInt(request, QStringLiteral("limit"), 100, 1, 500);
        QJsonObject buckets;
        for (const QString &status : {QStringLiteral("RUNNING"), QStringLiteral("NEEDS_INPUT"),
                                      QStringLiteral("COMPLETED_UNREAD"), QStringLiteral("FAILED_UNREAD"),
                                      QStringLiteral("VIEWED"), QStringLiteral("IGNORED")}) {
            bool hasMore = false;
            buckets.insert(status, QJsonObject{{QStringLiteral("tasks"), taskStore.tasksForStatusJson(status, limit, 0, &hasMore)},
                                               {QStringLiteral("hasMore"), hasMore}});
        }
        return jsonResponse(QJsonObject{{QStringLiteral("counts"), taskStore.summaryJson()},
                                        {QStringLiteral("buckets"), buckets}});
    });
    server.route("/api/tasks/read-all", QHttpServerRequest::Method::Options,
                 [] { return optionsResponse(); });
    server.route("/api/tasks/read-all", QHttpServerRequest::Method::Post,
                 [&taskStore] { return jsonResponse(QJsonObject{{QStringLiteral("success"), true},
                                                                  {QStringLiteral("count"), taskStore.markAllViewed()}}); });
    server.route("/api/tasks/<arg>/events", QHttpServerRequest::Method::Options,
                 [] { return optionsResponse(); });
    server.route("/api/tasks/<arg>/events", QHttpServerRequest::Method::Get,
                 [&taskStore](const qint64 id) {
        const QJsonObject task = taskStore.taskJson(id);
        if (task.isEmpty()) return jsonResponse(QJsonObject{{QStringLiteral("detail"), "task not found"}},
                                                QHttpServerResponse::StatusCode::NotFound);
        return jsonResponse(QJsonObject{{QStringLiteral("events"), taskStore.eventsJson(id)}});
    });
    server.route("/api/tasks/<arg>/ai-reply", QHttpServerRequest::Method::Options,
                 [] { return optionsResponse(); });
    server.route("/api/tasks/<arg>/ai-reply", QHttpServerRequest::Method::Get,
                 [&taskStore](const qint64 id) {
        if (taskStore.taskJson(id).isEmpty())
            return jsonResponse(QJsonObject{{QStringLiteral("detail"), "task not found"}}, QHttpServerResponse::StatusCode::NotFound);
        return jsonResponse(taskStore.aiReplyJson(id));
    });
    server.route("/api/tasks/<arg>", QHttpServerRequest::Method::Options,
                 [] { return optionsResponse(); });
    server.route("/api/tasks/<arg>", QHttpServerRequest::Method::Get,
                 [&taskStore](const qint64 id) {
        const QJsonObject task = taskStore.taskJson(id);
        if (task.isEmpty()) return jsonResponse(QJsonObject{{QStringLiteral("detail"), "task not found"}},
                                                QHttpServerResponse::StatusCode::NotFound);
        return jsonResponse(QJsonObject{{QStringLiteral("task"), task}});
    });
    server.route("/api/events", QHttpServerRequest::Method::Options,
                 [] { return optionsResponse(); });
    server.route("/api/events", QHttpServerRequest::Method::Post,
                 [&taskStore, &eventRateBuckets](const QHttpServerRequest &request) {
                     const QString client = request.remoteAddress().toString();
                     const qint64 now = QDateTime::currentMSecsSinceEpoch();
                     auto &bucket = eventRateBuckets[client];
                     while (!bucket.isEmpty() && now - bucket.head() >= 60'000) bucket.dequeue();
                     if (bucket.size() >= 60)
                         return jsonResponse(QJsonObject{{QStringLiteral("detail"), "rate limit exceeded"}},
                                             QHttpServerResponse::StatusCode::TooManyRequests);
                     if (request.body().size() > 1024 * 1024)
                         return jsonResponse(QJsonObject{{QStringLiteral("detail"), "event body too large"}},
                                             QHttpServerResponse::StatusCode::PayloadTooLarge);
                     QJsonParseError parseError;
                     const QJsonDocument event = QJsonDocument::fromJson(request.body(), &parseError);
                     if (parseError.error != QJsonParseError::NoError || !event.isObject())
                         return jsonResponse(QJsonObject{{QStringLiteral("detail"), "invalid event JSON"}},
                                             QHttpServerResponse::StatusCode::BadRequest);
                     bucket.enqueue(now);
                     const qint64 id = taskStore.ingestEvent(event.object());
                     if (id <= 0)
                         return jsonResponse(QJsonObject{{QStringLiteral("detail"), taskStore.lastError()}},
                                             QHttpServerResponse::StatusCode::BadRequest);
                     return jsonResponse(QJsonObject{{QStringLiteral("success"), true},
                                                     {QStringLiteral("taskId"), id},
                                                     {QStringLiteral("task"), taskStore.taskJson(id)}},
                                         QHttpServerResponse::StatusCode::Created);
                 });
    server.route("/api/tasks/<arg>/view", QHttpServerRequest::Method::Options,
                 [] { return optionsResponse(); });
    server.route("/api/tasks/<arg>/view", QHttpServerRequest::Method::Post,
                 [&taskStore](const qint64 id) {
                     const bool success = taskStore.setStatus(id, QStringLiteral("VIEWED"));
                     return jsonResponse(QJsonObject{{QStringLiteral("success"), success},
                                                     {QStringLiteral("task"), success ? taskStore.taskJson(id) : QJsonValue() }},
                                         success ? QHttpServerResponse::StatusCode::Ok : QHttpServerResponse::StatusCode::NotFound);
                 });
    server.route("/api/tasks/<arg>/ignore", QHttpServerRequest::Method::Options,
                 [] { return optionsResponse(); });
    server.route("/api/tasks/<arg>/ignore", QHttpServerRequest::Method::Post,
                 [&taskStore](const qint64 id) {
                     const bool success = taskStore.setStatus(id, QStringLiteral("IGNORED"));
                     return jsonResponse(QJsonObject{{QStringLiteral("success"), success},
                                                     {QStringLiteral("task"), success ? taskStore.taskJson(id) : QJsonValue()}},
                                         success ? QHttpServerResponse::StatusCode::Ok : QHttpServerResponse::StatusCode::NotFound);
                 });
    server.route("/api/tasks/<arg>", QHttpServerRequest::Method::Delete,
                 [&taskStore](const qint64 id) {
                     const bool success = taskStore.remove(id);
                     return jsonResponse(QJsonObject{{QStringLiteral("success"), success}},
                                         success ? QHttpServerResponse::StatusCode::Ok : QHttpServerResponse::StatusCode::NotFound);
                 });
    server.route("/api/tasks", QHttpServerRequest::Method::Delete,
                 [&taskStore](const QHttpServerRequest &request) {
                     if (request.query().queryItemValue(QStringLiteral("confirm")) != QStringLiteral("true"))
                         return jsonResponse(QJsonObject{{QStringLiteral("detail"), "confirm=true is required"}},
                                             QHttpServerResponse::StatusCode::BadRequest);
                     const QString requestedScope = request.query().queryItemValue(QStringLiteral("scope"));
                     const QString scope = requestedScope.isEmpty() ? QStringLiteral("all") : requestedScope;
                      if (!QStringList{QStringLiteral("completed"), QStringLiteral("queue"), QStringLiteral("history"), QStringLiteral("all")}.contains(scope))
                         return jsonResponse(QJsonObject{{QStringLiteral("detail"), "unknown scope"}},
                                             QHttpServerResponse::StatusCode::BadRequest);
                     return jsonResponse(QJsonObject{{QStringLiteral("success"), true},
                                                     {QStringLiteral("deleted"), taskStore.clear(scope)}});
                 });

    // 轻量集成状态接口：原生版不再依赖 Python，但仍让设置页能确认适配器目录是否存在。
    server.route("/api/integrations/status", [&taskStore] {
        const QString home = QDir::homePath();
        const QString extension = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("adapters/chatgpt-extension"));
        return jsonResponse(QJsonObject{
            {QStringLiteral("claudeCode"), QJsonObject{{QStringLiteral("installed"), QFileInfo(QDir(home).filePath(".claude/settings.json")).exists()},
                                                        {QStringLiteral("settingsPath"), QDir(home).filePath(".claude/settings.json")}}},
            {QStringLiteral("codex"), QJsonObject{{QStringLiteral("installed"), QFileInfo(QDir(home).filePath(".codex/config.toml")).exists()},
                                                  {QStringLiteral("configPath"), QDir(home).filePath(".codex/config.toml")},
                                                  {QStringLiteral("forwardTarget"), true}, {QStringLiteral("exeRunning"), false},
                                                  {QStringLiteral("stale"), false}, {QStringLiteral("processCount"), 0}}},
            {QStringLiteral("chatgpt"), QJsonObject{{QStringLiteral("installed"), QFileInfo(extension).exists()},
                                                    {QStringLiteral("lastHeartbeat"), QJsonValue()}, {QStringLiteral("version"), QJsonValue()},
                                                    {QStringLiteral("extensionDir"), extension}}},
            {QStringLiteral("backend"), QJsonObject{{QStringLiteral("version"), "0.2.0-native"},
                                                    {QStringLiteral("runtime"), "C++/Qt"}}},
            {QStringLiteral("storage"), taskStore.databasePath()}});
    });

    QTcpServer listener;
    if (!listener.listen(QHostAddress::LocalHost, 17891) || !server.bind(&listener)) return 2;

    QQmlApplicationEngine engine;
    QList<QQmlError> qmlWarnings;
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &app,
                     [&qmlWarnings](const QList<QQmlError> &warnings) {
                         qmlWarnings = warnings;
                         for (const QQmlError &warning : warnings)
                             qWarning().noquote() << warning.toString();
                     });
    engine.rootContext()->setContextProperty("taskStore", &taskStore);
    engine.rootContext()->setContextProperty("nativePort", listener.serverPort());
    engine.rootContext()->setContextProperty(
        "nativeResourceDir", QUrl::fromLocalFile(resourceDirectory()).toString());
    engine.loadFromModule("AiTaskHub", "Main");
    if (engine.rootObjects().isEmpty()) {
        QFile startupLog(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("qml-startup.log")));
        if (startupLog.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            for (const QQmlError &warning : qmlWarnings)
                startupLog.write(warning.toString().toUtf8() + '\n');
        }
        return 1;
    }

    auto *rootObject = engine.rootObjects().constFirst();
    auto *mainWindow = qobject_cast<QWindow *>(rootObject);
    OrbGestureFilter orbGestureFilter(mainWindow, rootObject, &app);
    if (mainWindow) mainWindow->installEventFilter(&orbGestureFilter);
    auto updateOrbMask = [rootObject, mainWindow] {
        if (!mainWindow) return;
        if (rootObject->property("orbMode").toBool() &&
            !rootObject->property("orbHoverExpanded").toBool()) {
            mainWindow->setMask(QRegion(QRect(QPoint(0, 0), mainWindow->size()), QRegion::Ellipse));
        } else {
            mainWindow->setMask(QRegion());
        }
    };
    if (mainWindow) {
        QObject::connect(mainWindow, &QWindow::widthChanged, &app,
                         [updateOrbMask](int) { QTimer::singleShot(0, updateOrbMask); });
        QObject::connect(mainWindow, &QWindow::heightChanged, &app,
                         [updateOrbMask](int) { QTimer::singleShot(0, updateOrbMask); });
    }
    updateOrbMask();

    QSystemTrayIcon tray(bundledAppIcon());
    tray.setToolTip(QStringLiteral("AI Task Hub · 原生模式"));
    QMenu trayMenu;
    QAction *showAction = trayMenu.addAction(QStringLiteral("打开任务中心"));
    QAction *hideAction = trayMenu.addAction(QStringLiteral("隐藏窗口"));
    trayMenu.addSeparator();
    QAction *quitAction = trayMenu.addAction(QStringLiteral("退出"));
    tray.setContextMenu(&trayMenu);
    tray.show();
    auto updateApplicationIcon = [&app, &tray, &taskStore] {
        const QString path = QUrl(taskStore.userIconPath()).toLocalFile();
        const QIcon icon = path.isEmpty() ? bundledAppIcon() : QIcon(path);
        app.setWindowIcon(icon);
        tray.setIcon(icon);
    };
    QObject::connect(&taskStore, &TaskStore::appearanceChanged, &app, updateApplicationIcon);
    updateApplicationIcon();
    auto showMainWindow = [&engine] {
        if (auto *window = qobject_cast<QWindow *>(engine.rootObjects().value(0))) {
            window->show(); window->raise(); window->requestActivate();
        }
    };
    QObject::connect(showAction, &QAction::triggered, &app, showMainWindow);
    QObject::connect(hideAction, &QAction::triggered, &app, [&engine] {
        if (auto *window = qobject_cast<QWindow *>(engine.rootObjects().value(0))) window->hide();
    });
    QObject::connect(quitAction, &QAction::triggered, &app, &QApplication::quit);
    QObject::connect(&tray, &QSystemTrayIcon::activated, &app,
                     [&showMainWindow](QSystemTrayIcon::ActivationReason reason) {
                         if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) showMainWindow();
                     });
    QObject::connect(&taskStore, &TaskStore::taskReceived, &tray,
                     [&tray](qint64, const QString &title, const QString &source, const QString &status) {
                         if (status == QStringLiteral("VIEWED") || status == QStringLiteral("IGNORED")) return;
                         tray.showMessage(QStringLiteral("%1 · 新任务").arg(source), title,
                                         QSystemTrayIcon::Information, 5000);
                     });
    QObject::connect(&app, &QApplication::aboutToQuit, &tray, &QSystemTrayIcon::hide);
    return app.exec();
}
