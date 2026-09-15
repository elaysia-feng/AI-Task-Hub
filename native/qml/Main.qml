import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

ApplicationWindow {
id: window
width: 1020
height: 660
minimumWidth: 860
minimumHeight: 540
visible: true
title: "AI Task Hub"
color: "transparent"
flags: Qt.FramelessWindowHint | Qt.Window

    property string
page: "queue"
    property string
searchText: ""
    property string
statusFilter: "ALL"
    property string
sourceFilter: "ALL"
    property string
sortOrder: "newest"
    property bool
darkMode: true
    property bool
orbMode: false
    property bool
orbHoverExpanded: false
    property bool
orbPanelToLeft: false
    property bool
orbPointerOnBall: false
    property int
orbTasksRevision: 0
    property var
normalFlags: Qt.FramelessWindowHint | Qt.Window
    property var
themeIds: ["default", "rei-ayanami", "tomo-ebizuka", "elaina", "mutsumi-wakaba", "sakiko-togawa", "yui-hirasawa", "mio-akiyama", "ritsu-tainaka", "tsumugi-kotobuki", "azusa-nakano", "ayaka-kamisato", "aemeath", "shorekeeper"]
    property var
themeNames: ["AI 看板娘", "绫波丽", "海老塚智", "伊蕾娜", "若叶睦", "丰川祥子", "平泽唯", "秋山澪", "田井中律", "琴吹紬", "中野梓", "神里绫华", "爱弥斯", "守岸人"]
    property int
selectedId: 0
    property var
selectedTask: ({})
    property var
selectedEvents: []

    readonly property color
canvasColor: darkMode ? "#111416" : "#f4f2ed"
    readonly property color
sidebarColor: darkMode ? "#99080a0e" : "#d9f3f4f8"
    readonly property color
cardColor: darkMode ? "#7c10121a" : "#d9ffffff"
    readonly property color
cardHoverColor: darkMode ? "#9c161a24" : "#ffffffff"
    readonly property color
borderColor: darkMode ? "#14ffffff" : "#1714192d"
    readonly property color
borderStrong: darkMode ? "#24ffffff" : "#2b14192d"
    readonly property color
primaryText: darkMode ? "#f1f0ec" : "#252a2b"
    readonly property color
secondaryText: darkMode ? "#aaa9a3" : "#626664"
    readonly property color
mutedText: darkMode ? "#969994" : "#666a67"
    readonly property color
accent: "#df7654"
    readonly property color
accentSoft: darkMode ? "#24df7654" : "#22df7654"
    readonly property color
accentLine: darkMode ? "#66df7654" : "#70df7654"
    readonly property color
success: "#22c55e"
    readonly property color
warning: "#f59e0b"
    readonly property color
danger: "#ef4444"

    function isQueueStatus(status) {
        return ["RUNNING", "NEEDS_INPUT", "COMPLETED_UNREAD", "FAILED_UNREAD"].indexOf(status) >= 0
    }

    function taskVisibleForView(task, historyPage) {
        if (!task || !task.status) return false
        if (historyPage ? isQueueStatus(task.status) : !isQueueStatus(task.status)) return false
        if (statusFilter !== "ALL" && task.status !== statusFilter) return false
        if (sourceFilter !== "ALL" && task.source !== sourceFilter) return false
        const keyword = searchText.trim().toLowerCase()
        if (!keyword) return true
        return (task.title || "").toLowerCase().indexOf(keyword) >= 0
                || (task.contentPreview || "").toLowerCase().indexOf(keyword) >= 0
                || (task.projectPath || "").toLowerCase().indexOf(keyword) >= 0
    }

    function visibleTaskCount(historyPage) {
        let count = 0
        for (const task of taskStore.tasks) {
            if (taskVisibleForView(task, historyPage)) count += 1
        }
        return count
    }

    function statusCount(status, historyPage) {
        let count = 0
        for (const task of taskStore.tasks) {
            if (task.status === status && (historyPage ? !isQueueStatus(task.status) : isQueueStatus(task.status))) count += 1
        }
        return count
    }

    function sourceCount(source, historyPage) {
        if (source === "ALL") return viewTotal(historyPage)
        let count = 0
        for (const task of taskStore.tasks) {
            if (task.source === source && (historyPage ? !isQueueStatus(task.status) : isQueueStatus(task.status))) count += 1
        }
        return count
    }

    function viewTotal(historyPage) {
        return historyPage ?
taskStore.historyCount : taskStore.queueCount
    }

    function statusLabel(status) {
        return ({RUNNING: "执行中",
NEEDS_INPUT: "等待输入",
COMPLETED_UNREAD: "已完成",
FAILED_UNREAD: "失败",
VIEWED: "已查看",
IGNORED: "已忽略"})[status] || status
    }

    function sourceLabel(source) {
        return ({CHATGPT: "ChatGPT 网页",
CLAUDE_CODE: "Claude Code",
CODEX: "Codex",
OTHER: "其他"})[source] || source
    }

    function sourceColor(source) {
        return source === "CHATGPT" ? "#10a37f" : (source === "CLAUDE_CODE" ? "#d97757" : (source === "CODEX" ? "#4f8ff7" : "#8b5cf6"))
    }

    function statusColor(status) {
        if (status === "RUNNING") return "#38bdf8"
        if (status === "NEEDS_INPUT") return warning
        if (status === "FAILED_UNREAD") return danger
        if (status === "COMPLETED_UNREAD" || status === "VIEWED") return success
        return mutedText
    }

    function formatTime(value) {
        if (!value) return "刚刚"
        return value.toString().replace("T", " ").replace("Z", "").slice(0, 19)
    }

    function summaryText(historyPage) {
        const total = viewTotal(historyPage)
        if (total <= 0) return ""
        if (historyPage) return "共 " + total + " 条记录"
        const needsInput = statusCount("NEEDS_INPUT", false)
        return needsInput > 0 ? total + " 个任务 · " + needsInput + " 个等待你的输入" : total + " 个任务待查看"
    }

    function orbPreviewTasks() {
        // 读取刷新令牌，确保新事件到达时悬停面板立即重绘。
        if (orbTasksRevision < 0) return []
        let result = []
        for (const task of taskStore.tasks) {
            if (isQueueStatus(task.status)) result.push(task)
        }
        result.sort(function(a, b) {
            const left = (a.createdAt || "").toString()
            const right = (b.createdAt || "").toString()
            return right.localeCompare(left)
        })
        return result.slice(0, 4)
    }

    function orbTaskTitle(task) {
        return (task && (task.title || task.contentPreview)) || "未命名任务"
    }

    function orbTaskPreview(task) {
        const preview = task && task.contentPreview ? task.contentPreview.toString().replace(/\s+/g, " ").trim() : ""
        return preview && preview !== orbTaskTitle(task) ? preview : "点击查看完整消息"
    }

    function selectTask(task) {
        if (!task) return
        selectedId = task.id
        selectedTask = taskStore.taskVariant(task.id)
        selectedEvents = taskStore.eventVariants(task.id)
    }

    function refreshSelection() {
        if (selectedId <= 0) return
        const next = taskStore.taskVariant(selectedId)
        if (next && next.id) {
            selectedTask = next
            selectedEvents = taskStore.eventVariants(selectedId)
        } else {
            selectedId = 0
            selectedTask = ({})
            selectedEvents = []
        }
    }

    function toggleTheme() {
        darkMode = !darkMode
    }

    function resetFilters() {
        searchText = ""
        statusFilter = "ALL"
        sourceFilter = "ALL"
        sortOrder = "newest"
        selectedId = 0
    }

    function enterOrb() {
        normalFlags = window.flags
        orbHoverExpanded = false
        orbPanelToLeft = false
        orbMode = true
        window.flags = Qt.FramelessWindowHint | Qt.Window | Qt.WindowStaysOnTopHint | Qt.Tool
        window.width = 52
        window.height = 52
        window.color = "transparent"
    }

    function leaveOrb() {
        orbHoverExpanded = false
        orbMode = false
        window.flags = normalFlags
        window.width = 1020
        window.height = 660
        window.color = "transparent"
        window.show()
        window.raise()
        window.requestActivate()
    }

    Connections {
target: taskStore
        function onTasksChanged() {
            window.refreshSelection()
            window.orbTasksRevision += 1
        }
    }

    // 壁纸只做一层轻暗角，保持旧版“图透出来、面板不糊”的观感。
Image {
anchors.fill: parent
visible: !window.orbMode
source: taskStore.wallpaperPath !== ""
                ?
taskStore.wallpaperPath
                : nativeResourceDir + "/themes/" + taskStore.themeId + "/wallpaper-" + (darkMode ? "dark" : "light") + ".png"
fillMode: Image.PreserveAspectCrop
scale: 1.06
opacity: 1
asynchronous: true
cache: true
    }
Rectangle {
anchors.fill: parent
visible: !window.orbMode
color: darkMode ? "#52000000" : "#2affffff"
    }
Rectangle {
anchors.fill: parent
visible: !window.orbMode
color: "transparent"
border.width: 1
border.color: darkMode ? "#18ffffff" : "#1814192d"
    }

    // 轻量悬浮球：保留旧版“悬停展开任务概览”的行为，球面仍由原生窗口负责拖动。
    Item {
        id: orbLayer
        anchors.fill: parent
        visible: window.orbMode

        Rectangle {
            id: orbPreviewPanel
            visible: window.orbHoverExpanded
            x: window.orbPanelToLeft ? 8 : 68
            y: 8
            width: parent.width - 76
            height: parent.height - 16
            radius: 18
            color: darkMode ? "#e8141a22" : "#f4ffffff"
            border.width: 1
            border.color: darkMode ? "#28ffffff" : "#3014192d"
            z: 1

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 14
                spacing: 8

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 8
                    Label {
                        text: "任务概览"
                        color: primaryText
                        font.pixelSize: 14
                        font.bold: true
                    }
                    Label {
                        Layout.fillWidth: true
                        text: taskStore.queueCount > 0 ? taskStore.queueCount + " 条待处理消息" : "暂无待处理消息"
                        color: secondaryText
                        font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                }

                RowLayout {
                    Layout.fillWidth: true
                    spacing: 6
                    Repeater {
                        model: [
                            {label: "执行中", code: "RUNNING", color: "#38bdf8", fill: "#2638bdf8"},
                            {label: "待输入", code: "NEEDS_INPUT", color: "#f59e0b", fill: "#26f59e0b"},
                            {label: "已完成", code: "COMPLETED_UNREAD", color: "#22c55e", fill: "#2622c55e"},
                            {label: "失败", code: "FAILED_UNREAD", color: "#ef4444", fill: "#26ef4444"}
                        ]
                        delegate: Rectangle {
                            visible: window.statusCount(modelData.code, false) > 0
                            Layout.preferredWidth: statusPreviewLabel.implicitWidth + 18
                            Layout.preferredHeight: 22
                            radius: 11
                            color: modelData.fill
                            border.color: modelData.color
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 7
                                anchors.rightMargin: 7
                                spacing: 5
                                Rectangle {
                                    width: 5
                                    height: 5
                                    radius: 3
                                    color: modelData.color
                                }
                                Label {
                                    id: statusPreviewLabel
                                    text: modelData.label + " " + window.statusCount(modelData.code, false)
                                    color: modelData.color
                                    font.pixelSize: 10
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: borderColor
                }

                ColumnLayout {
                    id: orbPreviewTaskList
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    spacing: 6

                    Repeater {
                        model: window.orbPreviewTasks()
                        delegate: Rectangle {
                            id: orbPreviewTask
                            property var task: modelData
                            Layout.fillWidth: true
                            Layout.preferredHeight: 48
                            radius: 10
                            color: orbPreviewMouse.containsMouse ? cardHoverColor : cardColor
                            border.color: orbPreviewMouse.containsMouse ? borderStrong : borderColor

                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 9
                                anchors.rightMargin: 8
                                spacing: 7
                                Rectangle {
                                    Layout.preferredWidth: 4
                                    Layout.preferredHeight: 30
                                    radius: 2
                                    color: window.statusColor(orbPreviewTask.task.status)
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 5
                                        Label {
                                            text: window.sourceLabel(orbPreviewTask.task.source)
                                            color: window.sourceColor(orbPreviewTask.task.source)
                                            font.pixelSize: 10
                                            font.bold: true
                                        }
                                        Label {
                                            Layout.fillWidth: true
                                            text: window.statusLabel(orbPreviewTask.task.status)
                                            color: mutedText
                                            font.pixelSize: 10
                                            horizontalAlignment: Text.AlignRight
                                            elide: Text.ElideRight
                                        }
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: window.orbTaskTitle(orbPreviewTask.task)
                                        color: primaryText
                                        font.pixelSize: 11
                                        font.bold: true
                                        elide: Text.ElideRight
                                    }
                                    Label {
                                        Layout.fillWidth: true
                                        text: window.orbTaskPreview(orbPreviewTask.task)
                                        color: secondaryText
                                        font.pixelSize: 10
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                            MouseArea {
                                id: orbPreviewMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    window.selectTask(orbPreviewTask.task)
                                    window.leaveOrb()
                                }
                            }
                        }
                    }

                    Label {
                        visible: window.orbPreviewTasks().length === 0
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        text: "当前没有运行中或待查看的任务"
                        color: mutedText
                        font.pixelSize: 11
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                Label {
                    Layout.fillWidth: true
                    text: "点击任务查看详情 · 点击小球打开完整面板"
                    color: mutedText
                    font.pixelSize: 10
                    elide: Text.ElideRight
                }
            }
        }

        Item {
            id: orbContent
            width: 52
            height: 52
            x: window.orbHoverExpanded
               ? (window.orbPanelToLeft ? parent.width - width - 8 : 8)
               : (parent.width - width) / 2
            y: (parent.height - height) / 2
            scale: orbMouse.pressed ? 0.96 : (orbMouse.containsMouse ? 1.025 : 1)
            z: 2
            Behavior on scale {
                NumberAnimation {
                    duration: 130
                    easing.type: Easing.OutCubic
                }
            }
            Rectangle {
                anchors.fill: parent
                radius: width / 2
                color: "#1fdf7654"
                border.width: 1
                border.color: orbMouse.containsMouse ? "#a6df7654" : "#5adf7654"
                Behavior on border.color {
                    ColorAnimation { duration: 130 }
                }
            }
            Rectangle {
                id: orbBall
                width: 44
                height: 44
                anchors.centerIn: parent
                radius: 22
                color: "#ee141a22"
                border.width: 1
                border.color: "#8cffffff"
                clip: true
                CircularImage {
                    anchors.fill: parent
                    anchors.margins: 1
                    source: taskStore.userIconPath !== ""
                            ? taskStore.userIconPath
                            : nativeResourceDir + "/presets/default.png"
                }
                Rectangle {
                    anchors.fill: parent
                    gradient: Gradient {
                        GradientStop { position: 0.0; color: "#06000000" }
                        GradientStop { position: 0.55; color: "#00000000" }
                        GradientStop { position: 1.0; color: "#24000000" }
                    }
                }
                Rectangle {
                    anchors.fill: parent
                    radius: 22
                    color: "transparent"
                    border.width: 1
                    border.color: "#99ffffff"
                }
            }
            Rectangle {
                visible: taskStore.queueCount > 0
                width: Math.max(17, countText.implicitWidth + 8)
                height: 17
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.rightMargin: 6
                anchors.topMargin: 6
                radius: height / 2
                color: danger
                border.width: 1
                border.color: "#cc141a22"
                Text {
                    id: countText
                    anchors.centerIn: parent
                    text: taskStore.queueCount > 99 ? "99+" : taskStore.queueCount
                    color: "white"
                    font.pixelSize: 9
                    font.bold: true
                }
            }
            MouseArea {
                id: orbMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: pressed ? Qt.ClosedHandCursor : Qt.OpenHandCursor
            }
        }
    }
ColumnLayout {
anchors.fill: parent
visible: !window.orbMode
spacing: 0
Rectangle {
id: titlebar
Layout.fillWidth: true
Layout.preferredHeight: 46
color: darkMode ? "#55111416" : "#66f4f2ed"
border.color: darkMode ? "#0dffffff" : "#1214192d"
MouseArea {
anchors.fill: parent
z: -1
onPressed: window.startSystemMove()
            }
RowLayout {
anchors.fill: parent
anchors.leftMargin: 14
anchors.rightMargin: 8
spacing: 9
Rectangle {
width: 26
height: 26
radius: 8
color: darkMode ? "#20252a" : "#b8ffffff"
border.color: borderColor
clip: true
Image {
anchors.fill: parent
source: taskStore.userIconPath !== "" ?
taskStore.userIconPath : nativeResourceDir + "/presets/default.png"
fillMode: Image.PreserveAspectCrop
asynchronous: true
                    }
                }
Label {
text: "AI Task Hub"
color: primaryText
font.family: "Segoe UI Variable"
font.pixelSize: 14
font.bold: true }
Label {
text: "多 AI 平台任务中心"
color: mutedText
font.family: "Segoe UI Variable"
font.pixelSize: 11 }
                Item {
Layout.fillWidth: true }
Rectangle {
Layout.preferredWidth: 68
Layout.preferredHeight: 25
radius: 13
color: cardColor
border.color: borderColor
RowLayout {
anchors.fill: parent
anchors.leftMargin: 9
anchors.rightMargin: 8
spacing: 6
Rectangle {
width: 6
height: 6
radius: 3
color: taskStore.ready ?
success : danger }
Label {
text: taskStore.ready ? "已连接" : "离线"
color: secondaryText
font.pixelSize: 10
Layout.fillWidth: true }
                    }
                }
ToolButton {
id: themeButton
implicitWidth: 30
implicitHeight: 30
text: darkMode ? "☼" : "☾"
onClicked: window.toggleTheme()
background: Rectangle {
radius: 15
color: themeButton.hovered ?
cardHoverColor : "transparent" }
contentItem: Label {
text: themeButton.text
color: secondaryText
font.pixelSize: 15
horizontalAlignment: Text.AlignHCenter
verticalAlignment: Text.AlignVCenter }
                }
ToolButton {
id: orbButton
implicitWidth: 30
implicitHeight: 30
text: "◎"
onClicked: window.enterOrb()
background: Rectangle {
radius: 15
color: orbButton.hovered ?
accentSoft : "transparent" }
contentItem: Label {
text: orbButton.text
color: secondaryText
font.pixelSize: 16
horizontalAlignment: Text.AlignHCenter
verticalAlignment: Text.AlignVCenter }
                }
ToolButton {
id: minimizeButton
implicitWidth: 30
implicitHeight: 30
text: "−"
onClicked: window.enterOrb()
background: Rectangle {
radius: 15
color: minimizeButton.hovered ?
cardHoverColor : "transparent" }
contentItem: Label {
text: minimizeButton.text
color: secondaryText
font.pixelSize: 17
horizontalAlignment: Text.AlignHCenter
verticalAlignment: Text.AlignVCenter }
                }
ToolButton {
id: closeButton
implicitWidth: 30
implicitHeight: 30
text: "×"
onClicked: window.enterOrb()
background: Rectangle {
radius: 15
color: closeButton.hovered ? "#e81123" : "transparent" }
contentItem: Label {
text: closeButton.text
color: closeButton.hovered ? "white" : secondaryText
font.pixelSize: 18
horizontalAlignment: Text.AlignHCenter
verticalAlignment: Text.AlignVCenter }
                }
            }
        }
RowLayout {
Layout.fillWidth: true
Layout.fillHeight: true
spacing: 0
Rectangle {
id: sidebar
Layout.preferredWidth: 180
Layout.fillHeight: true
color: sidebarColor
border.color: borderColor
ColumnLayout {
anchors.fill: parent
anchors.leftMargin: 14
anchors.rightMargin: 14
anchors.topMargin: 18
anchors.bottomMargin: 14
spacing: 5
Label {
text: "任务"
color: mutedText
font.family: "Segoe UI Variable"
font.pixelSize: 11
font.bold: true
leftPadding: 10
bottomPadding: 7 }

                    component
NavItem: Item {
id: navItem
                        property string
itemPage: "queue"
                        property string
itemText: ""
                        property string
itemIcon: ""
Layout.fillWidth: true
Layout.preferredHeight: 40
Rectangle {
anchors.fill: parent
radius: 20
color: window.page === navItem.itemPage ?
accentSoft : "transparent"
border.color: window.page === navItem.itemPage ?
accentLine : "transparent" }
Rectangle {
visible: window.page === navItem.itemPage
width: 3
height: parent.height - 13
anchors.left: parent.left
anchors.verticalCenter: parent.verticalCenter
radius: 2
color: accent }
RowLayout {
anchors.fill: parent
anchors.leftMargin: 12
anchors.rightMargin: 10
spacing: 9
Label {
text: navItem.itemIcon
color: window.page === navItem.itemPage ?
primaryText : secondaryText
font.family: "Segoe UI Symbol"
font.pixelSize: 15
Layout.preferredWidth: 16
horizontalAlignment: Text.AlignHCenter }
Label {
text: navItem.itemText
color: primaryText
font.pixelSize: 12
Layout.fillWidth: true }
Rectangle {
visible: navItem.itemPage === "queue"
width: Math.max(19, queueCountText.implicitWidth + 12)
height: 18
radius: 9
color: taskStore.queueCount > 0 ?
accent : "transparent"
Label {
id: queueCountText
anchors.centerIn: parent
text: taskStore.queueCount
color: taskStore.queueCount > 0 ? "white" : mutedText
font.pixelSize: 11
font.bold: true }
                            }
Label {
visible: navItem.itemPage === "history"
text: taskStore.historyCount
color: mutedText
font.pixelSize: 11 }
                        }
MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                window.page = navItem.itemPage
                                window.resetFilters()
                            }
                        }
                    }
NavItem {
itemPage: "queue"
itemText: "待处理"
itemIcon: "▱" }
NavItem {
itemPage: "history"
itemText: "历史"
itemIcon: "◷" }
NavItem {
itemPage: "settings"
itemText: "设置"
itemIcon: "⚙" }
                    Item {
Layout.fillHeight: true }
Rectangle {
Layout.fillWidth: true
Layout.preferredHeight: 1
color: borderColor }
ColumnLayout {
Layout.fillWidth: true
Layout.preferredHeight: 86
spacing: 3
RowLayout {
Layout.fillWidth: true
spacing: 6
Rectangle {
width: 6
height: 6
radius: 3
color: taskStore.ready ?
success : danger }
Label {
text: taskStore.ready ? "本地服务在线" : "本地服务离线"
color: primaryText
font.pixelSize: 12
Layout.fillWidth: true } }
Label {
text: "127.0.0.1:" + nativePort
color: secondaryText
font.pixelSize: 11 }
Label {
text: taskStore.ready ? "事件驱动 · 本地运行" : "正在初始化 SQLite"
color: mutedText
font.pixelSize: 10
elide: Text.ElideRight
Layout.fillWidth: true }
Label {
text: "Claude Code · Codex ·\nChatGPT"
color: mutedText
font.pixelSize: 11
lineHeight: 1.45
Layout.fillWidth: true
wrapMode: Text.WordWrap
leftPadding: 10 }
                }
            }
        }

            Item {
id: content
Layout.fillWidth: true
Layout.fillHeight: true
clip: true
StackLayout {
anchors.fill: parent
anchors.leftMargin: 22
anchors.rightMargin: 22
anchors.topMargin: 22
anchors.bottomMargin: 22
currentIndex: window.page === "queue" ? 0 : (window.page === "history" ? 1 : 2)

                    Item {
id: queuePage
TaskView {
anchors.fill: parent
historyPage: false } }
                    Item {
id: historyPage
TaskView {
anchors.fill: parent
historyPage: true } }
SettingsView {
anchors.fill: parent }
                }
            }
        }
    }

    component
GhostButton: Button {
id: ghostButton
implicitHeight: 34
leftPadding: 14
rightPadding: 14
padding: 0
background: Rectangle {
radius: 17
color: ghostButton.hovered ?
cardHoverColor : cardColor
border.color: ghostButton.hovered ?
borderStrong : borderColor }
contentItem: Label {
text: ghostButton.text
color: ghostButton.enabled ?
secondaryText : mutedText
font.pixelSize: 12
horizontalAlignment: Text.AlignHCenter
verticalAlignment: Text.AlignVCenter }
    }

    component
PrimaryButton: Button {
id: primaryButton
implicitHeight: 34
leftPadding: 15
rightPadding: 15
padding: 0
background: Rectangle {
radius: 17
color: primaryButton.hovered ? "#3adf7654" : accentSoft
border.color: accentLine }
contentItem: Label {
text: primaryButton.text
color: darkMode ? "#f0a184" : "#b95c40"
font.pixelSize: 12
horizontalAlignment: Text.AlignHCenter
verticalAlignment: Text.AlignVCenter }
    }

    component
MiniButton: Button {
id: miniButton
implicitHeight: 28
leftPadding: 10
rightPadding: 10
padding: 0
background: Rectangle {
radius: 14
color: miniButton.hovered ?
cardHoverColor : "transparent"
border.color: borderColor }
contentItem: Label {
text: miniButton.text
color: miniButton.enabled ?
secondaryText : mutedText
font.pixelSize: 11
horizontalAlignment: Text.AlignHCenter
verticalAlignment: Text.AlignVCenter }
    }

    component
SectionCard: Rectangle {
id: sectionCard
        property int
cardHeight: 0
width: parent ?
parent.width : 0
height: sectionCard.cardHeight > 0 ?
sectionCard.cardHeight : childrenRect.height + 40
radius: 18
color: cardColor
border.color: borderColor
    }

    component
FilterBar: RowLayout {
id: filterBar
        property bool
historyPage: false
width: parent ?
parent.width : 0
spacing: 10
Rectangle {
Layout.fillWidth: true
Layout.preferredHeight: 40
radius: 20
color: cardColor
border.color: borderColor
RowLayout {
anchors.fill: parent
anchors.leftMargin: 13
anchors.rightMargin: 8
spacing: 7
Label {
text: "⌕"
color: mutedText
font.pixelSize: 19 }
                TextField {
id: searchField
Layout.fillWidth: true
text: window.searchText
placeholderText: "搜索标题 / 路径 / 摘要…"
color: primaryText
placeholderTextColor: mutedText
font.pixelSize: 12
background: null
onTextChanged: window.searchText = text
                }
ToolButton {
visible: searchField.text.length > 0
implicitWidth: 24
implicitHeight: 24
text: "×"
onClicked: {
                        window.searchText = ""
                        searchField.text = ""
                    }
background: Rectangle {
radius: 12
color: "transparent" }
contentItem: Label {
text: searchField.text.length > 0 ? "×" : ""
color: mutedText
font.pixelSize: 15
horizontalAlignment: Text.AlignHCenter
verticalAlignment: Text.AlignVCenter }
                }
Label {
visible: searchField.text.length === 0
text: "Ctrl K"
color: mutedText
font.pixelSize: 10
padding: 4
background: Rectangle {
radius: 6
color: darkMode ? "#0affffff" : "#0a14192d"
border.color: borderColor } }
            }
        }
        ColumnLayout {
spacing: 4
Label {
text: "排序"
color: mutedText
font.pixelSize: 10
leftPadding: 8 }
ComboBox {
id: sortCombo
Layout.preferredWidth: 112
Layout.preferredHeight: 40
model: ["newest", "oldest"]
currentIndex: window.sortOrder === "oldest" ? 1 : 0
onActivated: window.sortOrder = currentText
background: Rectangle {
radius: 20
color: cardColor
border.color: borderColor }
contentItem: Label {
text: sortCombo.currentText === "oldest" ? "最早优先" : "最新优先"
color: primaryText
font.pixelSize: 12
verticalAlignment: Text.AlignVCenter
leftPadding: 15
rightPadding: 24 }
            }
        }
    }

    component
StatusFilters: Flow {
id: statusFilters
        property bool
historyPage: false
width: parent ?
parent.width : 0
height: childrenRect.height
spacing: 7
        property var
queueOptions: [
            {code: "ALL",
label: "全部",
color: mutedText},
            {code: "RUNNING",
label: "执行中",
color: "#38bdf8"},
            {code: "NEEDS_INPUT",
label: "等待输入",
color: warning},
            {code: "COMPLETED_UNREAD",
label: "已完成",
color: success},
            {code: "FAILED_UNREAD",
label: "失败",
color: danger}
        ]
        property var
historyOptions: [
            {code: "ALL",
label: "全部",
color: mutedText},
            {code: "VIEWED",
label: "已查看",
color: success},
            {code: "IGNORED",
label: "已忽略",
color: mutedText}
        ]
Repeater {
model: statusFilters.historyPage ?
statusFilters.historyOptions : statusFilters.queueOptions
delegate: Item {
id: filterItem
                property var
option: modelData
width: filterLabel.implicitWidth + filterCount.implicitWidth + 33
height: 30
Rectangle {
anchors.fill: parent
radius: 15
color: window.statusFilter === filterItem.option.code ?
cardColor : "transparent"
border.color: window.statusFilter === filterItem.option.code ?
borderStrong : "transparent" }
RowLayout {
anchors.fill: parent
anchors.leftMargin: 11
anchors.rightMargin: 10
spacing: 7
Rectangle {
width: 6
height: 6
radius: 3
color: filterItem.option.color }
Label {
id: filterLabel
text: filterItem.option.label
color: window.statusFilter === filterItem.option.code ?
primaryText : mutedText
font.pixelSize: 12 }
Label {
id: filterCount
text: filterItem.option.code === "ALL" ? window.viewTotal(filterItem.historyPage) : window.statusCount(filterItem.option.code, filterItem.historyPage)
color: mutedText
font.pixelSize: 11 }
                }
MouseArea {
                    anchors.fill: parent
                    onClicked: {
                        window.statusFilter = filterItem.option.code
                        window.selectedId = 0
                    }
                }
            }
        }
    }

    component
SourceFilters: Flow {
id: sourceFilters
        property bool
historyPage: false
width: parent ?
parent.width : 0
height: childrenRect.height
spacing: 7
        property var
options: [
            {code: "ALL",
label: "全部来源",
color: mutedText},
            {code: "CHATGPT",
label: "ChatGPT 网页",
color: "#10a37f"},
            {code: "CLAUDE_CODE",
label: "Claude Code",
color: "#d97757"},
            {code: "CODEX",
label: "Codex",
color: "#4f8ff7"},
            {code: "OTHER",
label: "其他",
color: "#8b5cf6"}
        ]
Repeater {
model: sourceFilters.options
delegate: Item {
id: sourceItem
                property var
option: modelData
width: sourceName.implicitWidth + sourceCountLabel.implicitWidth + 37
height: 30
Rectangle {
anchors.fill: parent
radius: 15
color: window.sourceFilter === sourceItem.option.code ?
cardColor : "transparent"
border.color: window.sourceFilter === sourceItem.option.code ?
borderStrong : "transparent" }
RowLayout {
anchors.fill: parent
anchors.leftMargin: 11
anchors.rightMargin: 10
spacing: 7
Rectangle {
width: 6
height: 6
radius: 3
color: sourceItem.option.color }
Label {
id: sourceName
text: sourceItem.option.label
color: window.sourceFilter === sourceItem.option.code ?
primaryText : mutedText
font.pixelSize: 12 }
Label {
id: sourceCountLabel
text: window.sourceCount(sourceItem.option.code, sourceFilters.historyPage)
color: mutedText
font.pixelSize: 11 }
                }
MouseArea {
anchors.fill: parent
onClicked: {
                        window.sourceFilter = sourceItem.option.code
                        window.selectedId = 0
                    }
                }
            }
        }
    }

    component
TaskView: Item {
id: taskView
        property bool
historyPage: false
ColumnLayout {
anchors.fill: parent
spacing: 12
RowLayout {
Layout.fillWidth: true
Layout.preferredHeight: 38
ColumnLayout {
spacing: 2
Label {
text: taskView.historyPage ? "历史" : "待处理"
color: primaryText
font.family: "Segoe UI Variable Display"
font.pixelSize: 22
font.bold: true }
Label {
visible: window.summaryText(taskView.historyPage) !== ""
text: window.summaryText(taskView.historyPage)
color: secondaryText
font.pixelSize: 12 }
                }
                Item {
Layout.fillWidth: true }
GhostButton {
text: "一键已读"
visible: !taskView.historyPage
enabled: taskStore.queueCount > 0
onClicked: taskStore.markAllViewed() }
GhostButton {
text: "删除已完成"
visible: !taskView.historyPage
enabled: window.statusCount("COMPLETED_UNREAD", false) > 0
onClicked: taskStore.clear("completed") }
GhostButton {
text: taskView.historyPage ? "清理历史" : "清理待处理"
enabled: window.viewTotal(taskView.historyPage) > 0
onClicked: taskStore.clear(taskView.historyPage ? "history" : "queue") }
            }

            StatusFilters {
historyPage: taskView.historyPage
Layout.fillWidth: true }
            SourceFilters {
historyPage: taskView.historyPage
Layout.fillWidth: true }
            FilterBar {
historyPage: taskView.historyPage
Layout.fillWidth: true }

            Item {
id: taskArea
Layout.fillWidth: true
Layout.fillHeight: true
Flickable {
id: taskScroll
anchors.left: parent.left
anchors.right: detailPanel.visible ?
detailPanel.left : parent.right
anchors.rightMargin: detailPanel.visible ? 16 : 0
anchors.top: parent.top
anchors.bottom: parent.bottom
clip: true
contentWidth: width
contentHeight: Math.max(taskFlow.height, height)
Flow {
id: taskFlow
width: taskScroll.width
height: childrenRect.height
spacing: 14
Repeater {
model: taskStore.tasks
delegate: Item {
id: taskCard
                                property var
task: modelData
                                property bool
matches: window.taskVisibleForView(task, taskView.historyPage)
width: matches ? (taskFlow.width >= 620 ? (taskFlow.width - 14) / 2 : taskFlow.width) : 0
height: matches ? 146 : 0
visible: matches
Rectangle {
anchors.fill: parent
radius: 18
color: cardMouse.containsMouse ?
cardHoverColor : cardColor
border.width: window.selectedId === taskCard.task.id ? 1 : 0
border.color: accentLine
Behavior on color { ColorAnimation {
duration: 140 } } }
Rectangle {
width: 4
height: parent.height - 32
anchors.left: parent.left
anchors.leftMargin: 8
anchors.verticalCenter: parent.verticalCenter
radius: 2
color: window.statusColor(taskCard.task.status)
opacity: 0.95 }
ColumnLayout {
anchors.fill: parent
anchors.leftMargin: 21
anchors.rightMargin: 15
anchors.topMargin: 14
anchors.bottomMargin: 12
spacing: 5
RowLayout {
Layout.fillWidth: true
spacing: 7
Rectangle {
id: sourcePill
property color sourceTint: window.sourceColor(taskCard.task.source)
Layout.preferredWidth: sourceName.implicitWidth + 24
Layout.preferredHeight: 22
radius: 11
color: Qt.rgba(sourceTint.r, sourceTint.g, sourceTint.b, 0.14)
border.color: Qt.rgba(sourceTint.r, sourceTint.g, sourceTint.b, 0.30)
RowLayout {
anchors.fill: parent
anchors.leftMargin: 8
anchors.rightMargin: 8
spacing: 5
Rectangle {
Layout.preferredWidth: 5
Layout.preferredHeight: 5
radius: 3
color: window.sourceColor(taskCard.task.source) }
Label {
id: sourceName
text: window.sourceLabel(taskCard.task.source)
color: window.sourceColor(taskCard.task.source)
font.pixelSize: 11
font.bold: true
Layout.fillWidth: true }
                }
            }
Label {
text: "·"
color: mutedText
font.pixelSize: 12 }
Label {
text: window.statusLabel(taskCard.task.status)
color: window.statusColor(taskCard.task.status)
font.pixelSize: 11 }
                                        Item {
Layout.fillWidth: true }
Label {
text: window.formatTime(taskCard.task.completedAt || taskCard.task.createdAt)
color: mutedText
font.pixelSize: 10 }
                                    }
Label {
text: taskCard.task.title || "未命名任务"
color: primaryText
font.pixelSize: 14
font.bold: true
elide: Text.ElideRight
Layout.fillWidth: true }
Label {
text: taskCard.task.contentPreview || "暂无摘要"
color: secondaryText
font.pixelSize: 12
elide: Text.ElideRight
Layout.fillWidth: true
maximumLineCount: 1 }
RowLayout {
Layout.fillWidth: true
Layout.fillHeight: true
z: 2
Label {
text: taskCard.task.projectPath || ""
color: mutedText
font.family: "Cascadia Code"
font.pixelSize: 10
elide: Text.ElideMiddle
Layout.fillWidth: true }
MiniButton {
visible: cardMouse.containsMouse
text: taskView.historyPage ? "删除" : "忽略"
onClicked: taskView.historyPage ? taskStore.remove(taskCard.task.id) : taskStore.setStatus(taskCard.task.id, "IGNORED") }
PrimaryButton {
visible: cardMouse.containsMouse && !taskView.historyPage
text: "打开"
onClicked: taskStore.openTask(taskCard.task.id) }
                                    }
                                }
MouseArea {
id: cardMouse
anchors.fill: parent
z: 1
hoverEnabled: true
onClicked: window.selectTask(taskCard.task) }
                            }
                        }
                    }
                }

                Item {
anchors.fill: taskScroll
visible: window.visibleTaskCount(taskView.historyPage) === 0
                    Column {
anchors.centerIn: parent
spacing: 8
Rectangle {
width: 80
height: 80
anchors.horizontalCenter: parent.horizontalCenter
radius: 40
color: accentSoft
border.color: accentLine
Text {
anchors.centerIn: parent
text: "⌄"
color: "#ef9b7c"
font.pixelSize: 40 }
                        }
Label {
anchors.horizontalCenter: parent.horizontalCenter
text: taskView.historyPage ? "暂无历史任务" : "全部处理完毕"
color: primaryText
font.pixelSize: 14
font.bold: true }
Label {
anchors.horizontalCenter: parent.horizontalCenter
text: taskView.historyPage ? "已查看和已忽略的任务会保留在这里" : "新的 AI 任务完成时会实时推送到这里"
color: mutedText
font.pixelSize: 12
wrapMode: Text.WordWrap }
                    }
                }
Rectangle {
id: detailPanel
visible: window.selectedId > 0
anchors.right: parent.right
anchors.top: parent.top
anchors.bottom: parent.bottom
width: 360
radius: 18
color: cardColor
border.color: borderColor
ColumnLayout {
anchors.fill: parent
anchors.margins: 20
spacing: 12
RowLayout {
Layout.fillWidth: true
Label {
text: "任务详情"
color: primaryText
font.pixelSize: 14
font.bold: true
Layout.fillWidth: true }
ToolButton {
id: detailClose
implicitWidth: 26
implicitHeight: 26
text: "×"
onClicked: window.selectedId = 0
background: Rectangle {
radius: 13
color: detailClose.hovered ?
cardHoverColor : "transparent" }
contentItem: Label {
text: detailClose.text
color: secondaryText
font.pixelSize: 18
horizontalAlignment: Text.AlignHCenter
verticalAlignment: Text.AlignVCenter }
                            }
                        }
Rectangle {
Layout.fillWidth: true
height: 1
color: borderColor }
Label {
text: selectedTask.title || "未命名任务"
color: primaryText
font.pixelSize: 15
font.bold: true
wrapMode: Text.WordWrap
Layout.fillWidth: true }
RowLayout {
Layout.fillWidth: true
Label {
text: window.sourceLabel(selectedTask.source || "OTHER")
color: accent
font.pixelSize: 11
font.bold: true }
Label {
text: window.statusLabel(selectedTask.status || "")
color: window.statusColor(selectedTask.status || "")
font.pixelSize: 11 }
                            Item {
Layout.fillWidth: true }
Label {
text: window.formatTime(selectedTask.createdAt)
color: mutedText
font.pixelSize: 10 }
                        }
Label {
visible: !!selectedTask.contentPreview
text: selectedTask.contentPreview || ""
color: secondaryText
font.pixelSize: 12
wrapMode: Text.WordWrap
maximumLineCount: 5
Layout.fillWidth: true }
Rectangle {
Layout.fillWidth: true
height: 1
color: borderColor }
Label {
text: "生命周期"
color: mutedText
font.pixelSize: 12
font.bold: true }
ListView {
Layout.fillWidth: true
Layout.fillHeight: true
clip: true
spacing: 4
model: selectedEvents
delegate: RowLayout {
width: parent ?
parent.width : 300
spacing: 9
Rectangle {
width: 7
height: 7
radius: 4
color: accent
Layout.alignment: Qt.AlignTop
Layout.topMargin: 5 }
ColumnLayout {
Layout.fillWidth: true
spacing: 2
Label {
text: window.statusLabel((modelData.eventType || "").replace("TASK_", "")) || modelData.eventType
color: primaryText
font.pixelSize: 11 }
Label {
text: window.formatTime(modelData.occurredAt)
color: mutedText
font.pixelSize: 10 } }
                            }
                        }
RowLayout {
Layout.fillWidth: true
spacing: 7
PrimaryButton {
Layout.fillWidth: true
text: "打开"
enabled: !!(selectedTask.openUrl || selectedTask.projectPath)
onClicked: taskStore.openTask(window.selectedId) }
GhostButton {
text: "已读"
visible: selectedTask.status !== "VIEWED"
onClicked: {
                                taskStore.setStatus(window.selectedId, "VIEWED")
                                window.refreshSelection()
                            }
                        }
MiniButton {
text: "删除"
onClicked: {
                                taskStore.remove(window.selectedId)
                                window.selectedId = 0
                            }
                        }
                        }
                    }
                }
            }
        }
    }

    component
SettingsView: Flickable {
id: settingsView
contentWidth: width
contentHeight: settingsColumn.height
clip: true

        Column {
id: settingsColumn
width: settingsView.width
spacing: 26
Label {
text: "设置"
color: primaryText
font.family: "Segoe UI Variable Display"
font.pixelSize: 22
font.bold: true }

            Column {
width: parent.width
spacing: 10
Label {
text: "外观"
color: secondaryText
font.pixelSize: 13
font.bold: true }
SectionCard {
id: appearanceCard
height: 300
                    Column {
anchors.fill: parent
anchors.margins: 20
spacing: 13
Label {
text: "内置背景主题（自动适配 Light / Dark）"
color: mutedText
font.pixelSize: 12 }
Flow {
width: parent.width
height: childrenRect.height
spacing: 8
Repeater {
model: window.themeIds
delegate: Button {
id: themeChoice
width: 116
height: 56
                                    property bool
selected: taskStore.themeId === modelData
onClicked: taskStore.setTheme(modelData)
background: Rectangle {
radius: 14
color: themeChoice.selected ?
accentSoft : cardColor
border.color: themeChoice.selected ?
accentLine : borderColor }
contentItem: RowLayout {
spacing: 7
Image {
Layout.preferredWidth: 38
Layout.preferredHeight: 38
source: nativeResourceDir + "/presets/" + modelData + ".png"
fillMode: Image.PreserveAspectCrop
asynchronous: true }
Label {
text: window.themeNames[index]
color: primaryText
font.pixelSize: 11
wrapMode: Text.WordWrap
Layout.fillWidth: true
maximumLineCount: 2 }
                                    }
                                }
                            }
                        }
RowLayout {
spacing: 8
GhostButton {
text: "选择本地壁纸…"
onClicked: taskStore.pickWallpaper() }
GhostButton {
text: "恢复默认"
onClicked: {
                                taskStore.clearWallpaper()
                                taskStore.clearUserIcon()
                            }
                        }
Label {
text: taskStore.wallpaperPath !== "" ? "已使用本地壁纸" : "已使用内置主题"
color: mutedText
font.pixelSize: 11
Layout.fillWidth: true
horizontalAlignment: Text.AlignRight }
                        }
                    }
                }
            }

            Column {
width: parent.width
spacing: 10
Label {
text: "应用图标"
color: secondaryText
font.pixelSize: 13
font.bold: true }
SectionCard {
height: 300
                    Column {
anchors.fill: parent
anchors.margins: 20
spacing: 13
Label {
text: "内置应用头像（标题栏、悬浮球、任务栏和托盘同步）"
color: mutedText
font.pixelSize: 12 }
Flow {
width: parent.width
height: childrenRect.height
spacing: 8
Repeater {
model: window.themeIds
delegate: Button {
id: iconChoice
width: 116
height: 56
                                    property bool
selected: (taskStore.userIconPath === "" && modelData === "default") || taskStore.userIconPath.indexOf("icon-preset-" + modelData + ".png") >= 0
onClicked: taskStore.setUserIconPreset(modelData)
background: Rectangle {
radius: 14
color: iconChoice.selected ?
accentSoft : cardColor
border.color: iconChoice.selected ?
accentLine : borderColor }
contentItem: RowLayout {
spacing: 7
Image {
Layout.preferredWidth: 38
Layout.preferredHeight: 38
source: nativeResourceDir + "/presets/" + modelData + ".png"
fillMode: Image.PreserveAspectCrop
asynchronous: true }
Label {
text: window.themeNames[index]
color: primaryText
font.pixelSize: 11
wrapMode: Text.WordWrap
Layout.fillWidth: true
maximumLineCount: 2 }
                                    }
                                }
                            }
                        }
RowLayout {
spacing: 8
GhostButton {
text: "选择本地图片…"
onClicked: taskStore.pickUserIcon() }
GhostButton {
text: "恢复默认"
onClicked: taskStore.clearUserIcon() }
Label {
text: taskStore.userIconPath.indexOf("icon-preset-") >= 0 ? "已使用内置头像" : (taskStore.userIconPath !== "" ? "已使用本地头像" : "默认头像")
color: mutedText
font.pixelSize: 11
Layout.fillWidth: true
horizontalAlignment: Text.AlignRight }
                        }
                    }
                }
            }

            Column {
width: parent.width
spacing: 10
Label {
text: "接入集成"
color: secondaryText
font.pixelSize: 13
font.bold: true }
SectionCard {
height: 116
                    Column {
anchors.fill: parent
anchors.margins: 20
spacing: 9
RowLayout {
width: parent.width
Label {
text: "ChatGPT 网页 · Claude Code · Codex"
color: primaryText
font.pixelSize: 13
font.bold: true
Layout.fillWidth: true }
Label {
text: "统一协议"
color: success
font.pixelSize: 11
font.bold: true } }
Label {
text: "三种适配器继续使用同一份 AgentEvent 协议：POST /api/events。"
color: secondaryText
font.pixelSize: 12
wrapMode: Text.WordWrap
width: parent.width }
Label {
text: "来源：CHATGPT · CLAUDE_CODE · CODEX · OTHER    事件：started / needs_input / completed / failed"
color: mutedText
font.pixelSize: 11
wrapMode: Text.WordWrap
width: parent.width }
                    }
                }
            }

            Column {
width: parent.width
spacing: 10
Label {
text: "存储后端"
color: secondaryText
font.pixelSize: 13
font.bold: true }
SectionCard {
height: 116
                    Column {
anchors.fill: parent
anchors.margins: 20
spacing: 9
Label {
text: "SQLite WAL · 低内存模式"
color: primaryText
font.pixelSize: 13
font.bold: true }
Label {
text: taskStore.databasePath
color: secondaryText
font.pixelSize: 11
elide: Text.ElideMiddle
width: parent.width }
RowLayout {
spacing: 8
GhostButton {
text: "打开数据目录"
onClicked: taskStore.openDataDirectory() }
GhostButton {
text: "全部标记已读"
onClicked: taskStore.markAllViewed() }
MiniButton {
text: "清空全部"
enabled: taskStore.totalTasks > 0
onClicked: taskStore.clear("all") } }
                    }
                }
            }

            Column {
width: parent.width
spacing: 10
Label {
text: "运行状态"
color: secondaryText
font.pixelSize: 13
font.bold: true }
SectionCard {
height: 112
                    Column {
anchors.fill: parent
anchors.margins: 20
spacing: 9
RowLayout {
width: parent.width
Label {
text: "原生 C++ / Qt 6"
color: primaryText
font.pixelSize: 13
font.bold: true
Layout.fillWidth: true }
Label {
text: taskStore.ready ? "运行中" : "初始化失败"
color: taskStore.ready ?
success : danger
font.pixelSize: 11
font.bold: true } }
Label {
text: "单进程桌面 + HTTP + SQLite，不再拉起 Electron、Chromium 或 Python。"
color: secondaryText
font.pixelSize: 12
wrapMode: Text.WordWrap
width: parent.width }
RowLayout {
width: parent.width
Label {
text: "127.0.0.1:" + nativePort
color: mutedText
font.pixelSize: 11 }
Label {
text: "任务 " + taskStore.totalTasks + " · 队列 " + taskStore.queueCount
color: mutedText
font.pixelSize: 11 } Item {
Layout.fillWidth: true }
GhostButton {
text: darkMode ? "深色" : "浅色"
onClicked: window.toggleTheme() }
GhostButton {
text: "收起为悬浮球"
onClicked: window.enterOrb() } }
                    }
                }
            }
Label {
text: "AI Task Hub Native 0.2.0 · 数据保存在用户 AppData，卸载程序不会自动删除。"
color: mutedText
font.pixelSize: 11
width: parent.width }
        }
    }
}
