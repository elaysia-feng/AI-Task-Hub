param(
    [Parameter(Mandatory=$true)][string]$Executable,
    [Parameter(Mandatory=$true)][string]$OutputDirectory,
    [switch]$AppearanceOnly
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class HubUiVerify {
    [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct Point { public int X,Y; }
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls, string title);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd, out Rect rect);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr hwnd, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr hwnd, IntPtr after, int x,int y,int w,int h,uint flags);
    [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
    [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr hwnd, ref Point point);
    [DllImport("user32.dll")] public static extern bool GetCursorPos(out Point point);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr hwnd, int command);
    [DllImport("user32.dll")] public static extern bool UpdateWindow(IntPtr hwnd);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd, IntPtr hdcBlt, uint flags);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
[HubUiVerify]::SetProcessDPIAware() | Out-Null
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
if ([HubUiVerify]::FindWindow('AI_TASK_HUB_WIN32_WINDOW', 'AI Task Hub') -ne [IntPtr]::Zero) { throw '请先关闭已有任务中心，避免操作真实窗口' }
$databasePath = Join-Path ([IO.Path]::GetDirectoryName($Executable)) 'data.sqlite'
if (Test-Path -LiteralPath $databasePath) { throw 'GUI 验证目录必须是没有现有数据库的隔离目录' }
# 先放置空数据库文件，阻止便携版把真实 AppData 旧库迁移进测试目录；应用启动后会自动建表。
New-Item -ItemType File -Path $databasePath | Out-Null
$process = Start-Process -FilePath $Executable -WorkingDirectory (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path -PassThru
$cursor = New-Object HubUiVerify+Point
[HubUiVerify]::GetCursorPos([ref]$cursor) | Out-Null
$script:window = [IntPtr]::Zero
function Rect {
    $rect = New-Object HubUiVerify+Rect
    [HubUiVerify]::GetWindowRect($script:window, [ref]$rect) | Out-Null
    return $rect
}
function Send([uint32]$message, [int64]$wp = 0, [int64]$lp = 0) {
    [HubUiVerify]::SendMessage($script:window, $message, [IntPtr]$wp, [IntPtr]$lp) | Out-Null
}
function Activate-Window {
    [HubUiVerify]::ShowWindow($script:window, 5) | Out-Null
    [HubUiVerify]::BringWindowToTop($script:window) | Out-Null
    [HubUiVerify]::SetForegroundWindow($script:window) | Out-Null
    Start-Sleep -Milliseconds 200
}
function Move-Pointer([int]$x, [int]$y, [int]$wait = 150) {
    $rect = Rect
    $screenPoint = New-Object HubUiVerify+Point
    $screenPoint.X = $x; $screenPoint.Y = $y
    [HubUiVerify]::ClientToScreen($script:window, [ref]$screenPoint) | Out-Null
    [HubUiVerify]::SetCursorPos($screenPoint.X, $screenPoint.Y) | Out-Null
    Send 0x200 0 (($y -shl 16) -bor ($x -band 65535))
    # 命中区域随悬停状态重绘；同步处理一次 WM_PAINT，避免自动化点击落在上一帧命中表。
    [HubUiVerify]::UpdateWindow($script:window) | Out-Null
    Start-Sleep -Milliseconds $wait
}
function Click([int]$x, [int]$y) {
    Move-Pointer $x $y
    $point = ($y -shl 16) -bor ($x -band 65535)
    Send 0x201 1 $point
    Send 0x202 0 $point
    [HubUiVerify]::UpdateWindow($script:window) | Out-Null
    Start-Sleep -Milliseconds 220
}
function Click-Local([int]$x, [int]$y) {
    $point = ($y -shl 16) -bor ($x -band 65535)
    Send 0x201 1 $point
    Send 0x202 0 $point
    [HubUiVerify]::UpdateWindow($script:window) | Out-Null
}
function Resize([int]$width, [int]$height) {
    [HubUiVerify]::SetWindowPos($script:window, [IntPtr](-1), 40,40,$width,$height,0x40) | Out-Null
    Activate-Window
    Start-Sleep -Milliseconds 250
}
function Shot([string]$name) {
    $rect = Rect
    $bitmap = New-Object System.Drawing.Bitmap ($rect.Right-$rect.Left), ($rect.Bottom-$rect.Top)
    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $captured = $false
        try {
            # 交互桌面可用时优先截屏，得到与用户所见一致的结果。
            $graphics.CopyFromScreen($rect.Left, $rect.Top, 0, 0, $bitmap.Size)
            $captured = $true
        } catch {
            # 服务化/无桌面会话可能没有可用屏幕句柄，改用窗口自身绘制结果继续验证。
            $hdc = $graphics.GetHdc()
            try { $captured = [HubUiVerify]::PrintWindow($script:window, $hdc, 2) }
            finally { $graphics.ReleaseHdc($hdc) }
        }
        if (-not $captured) { throw '窗口截图失败：CopyFromScreen 与 PrintWindow 均未返回成功' }
        $bitmap.Save((Join-Path $OutputDirectory ($name + '.png')), [System.Drawing.Imaging.ImageFormat]::Png)
    } finally { $graphics.Dispose(); $bitmap.Dispose() }
}
function Memory([string]$name) {
    $process.Refresh()
    Write-Output "$name private=$([math]::Round($process.PrivateMemorySize64/1MB,1)) MiB; workingSet=$([math]::Round($process.WorkingSet64/1MB,1)) MiB"
}
try {
    for ($i=0; $i -lt 30; $i++) {
        Start-Sleep -Milliseconds 200
        $script:window = [HubUiVerify]::FindWindow('AI_TASK_HUB_WIN32_WINDOW', 'AI Task Hub')
        if ($script:window -ne [IntPtr]::Zero) { break }
    }
    if ($script:window -eq [IntPtr]::Zero) { throw 'GUI 未出现' }
    Activate-Window
    Start-Sleep -Milliseconds 500
    $initial = Rect
    if ($initial.Right-$initial.Left -ne 52) { throw '启动悬浮球尺寸异常' }
    if ($AppearanceOnly) {
        Move-Pointer 26 26
        Start-Sleep -Milliseconds 250
        Click 50 327
        Resize 1004 644
        Click 68 198
        $appearanceRect = Rect
        Write-Output "appearance-window=$($appearanceRect.Left),$($appearanceRect.Top),$($appearanceRect.Right),$($appearanceRect.Bottom)"
        Shot 'appearance-start'

        # 14 个内置预设在 1004 宽窗口中分为 3 行，向下滚动后点击第二行的守岸人头像。
        Move-Pointer 600 450
        Send 0x20A (-1200 * 65536) 0
        Start-Sleep -Milliseconds 250
        Click 400 500
        $iniPath = Join-Path ([IO.Path]::GetDirectoryName($Executable)) 'AI Task Hub.ini'
        $ini = Get-Content -LiteralPath $iniPath -Raw -Encoding utf8
        if ($ini -notmatch '(?m)^userIconPreset=shorekeeper\s*$') { throw '守岸人头像点击未落盘' }
        Shot 'appearance-icon-shorekeeper'

        # 回到顶部后点击第二行第一列的爱弥斯壁纸，确认壁纸选择同样会刷新并保存。
        Send 0x20A (1200 * 65536) 0
        Start-Sleep -Milliseconds 250
        Click 250 410
        $ini = Get-Content -LiteralPath $iniPath -Raw -Encoding utf8
        if ($ini -notmatch '(?m)^themeId=aemeath\s*$') { throw '爱弥斯壁纸点击未落盘' }
        Shot 'appearance-wallpaper-aemeath'

        # 在保留爱弥斯壁纸的情况下恢复头像，确认头像恢复不会误清壁纸。
        Send 0x20A (-1200 * 65536) 0
        Start-Sleep -Milliseconds 250
        Click 410 585
        $ini = Get-Content -LiteralPath $iniPath -Raw -Encoding utf8
        if ($ini -notmatch '(?m)^userIconPreset=\s*$' -or $ini -notmatch '(?m)^themeId=aemeath\s*$') {
            throw '恢复默认头像影响了壁纸主题'
        }
        Write-Output 'PASS appearance preset selection, scoped reset and image refresh'
        return
    }
    Shot '01-orb'
    Memory 'orb-start'
    # 测试库由调用方准备；只向此隔离程序发送模拟事件。
    $status = Invoke-RestMethod 'http://127.0.0.1:17891/api/status'
    if (-not $status.db.database.StartsWith([IO.Path]::GetDirectoryName($Executable))) { throw '服务并非隔离进程，停止测试' }
    foreach ($tool in @('claude-code', 'codex')) {
        $code = 0
        try { Invoke-WebRequest "http://127.0.0.1:17891/api/integrations/$tool/install" -Method Post -ContentType 'application/json' -Body '{}' -UseBasicParsing | Out-Null }
        catch { $code = [int]$_.Exception.Response.StatusCode }
        if ($code -ne 404) { throw 'HTTP 安装接口仍然开放' }
    }
    $idleClient = New-Object Net.Sockets.TcpClient
    try {
        $idleClient.Connect('127.0.0.1', 17891)
        $health = Invoke-RestMethod 'http://127.0.0.1:17891/api/health' -TimeoutSec 5
        if ($health.status -ne 'ok') { throw '空闲连接阻塞服务' }
    } finally { $idleClient.Dispose() }
    $rows = @(
        @{source='CODEX';externalTaskId='ui-one';eventType='TASK_COMPLETED';title='完善任务中心界面与交互';contentPreview='筛选准确命中，长文本和按钮保持独立布局。';replyText=('这是一段用于验证详情滚动的中文答复。' * 120)},
        @{source='CLAUDE_CODE';externalTaskId='ui-two';eventType='TASK_NEEDS_INPUT';title='等待确认接口实现方案';contentPreview='请选择下一步要执行的操作。'},
        @{source='CHATGPT';externalTaskId='ui-three';eventType='TASK_STARTED';title='GPT 网页任务示例';contentPreview='正在处理你的请求。'}
    )
    foreach ($row in $rows) {
        Invoke-RestMethod 'http://127.0.0.1:17891/api/events' -Method Post -ContentType 'application/json; charset=utf-8' -Body ([Text.Encoding]::UTF8.GetBytes(($row | ConvertTo-Json -Compress))) | Out-Null
    }
    Move-Pointer 26 26 20
    $expandedOrb = Rect
    if (($expandedOrb.Right - $expandedOrb.Left) -ne 240) { throw '悬浮球没有展开为概览面板' }
    # 直接向展开态窗口发送按钮点击，避免自动化光标坐标在窗口重定位时触发收起计时器。
    Click-Local 50 327
    Start-Sleep -Milliseconds 220
    Shot '02-panel-opened'
    Resize 1004 644
    Shot '03-tasks'
    Memory 'panel'
    Click 740 209
    Shot '04-codex-filter'
    Click 300 350
    Shot '05-detail'
    Move-Pointer 600 460
    $rect = Rect
    Send 0x20A (-1200 * 65536) ((($rect.Top+460) -shl 16) -bor ($rect.Left+600))
    Start-Sleep -Milliseconds 180
    Shot '06-detail-scroll'
    Send 0x100 27 0
    Click 68 198
    Shot '07-appearance'
    Move-Pointer 600 450
    Send 0x20A (-720 * 65536) 0
    Start-Sleep -Milliseconds 180
    Shot '08-appearance-scroll'
    Click 395 134
    Shot '09-integrations'
    Click 524 134
    Shot '10-notifications'
    Click 910 484
    Click 818 23
    Shot '11-light'
    # 先筛选 GPT 网页，避免任务排序或两列换行让固定坐标误点到其他来源。
    Click 68 110
    Click 430 209
    # 卡片动作常显，直接点击右下角忽略按钮，不依赖真实鼠标 hover。
    Click-Local 484 417
    Start-Sleep -Milliseconds 220
    $afterIgnore = Invoke-RestMethod 'http://127.0.0.1:17891/api/tasks?view=history&source=CHATGPT'
    if ($afterIgnore.tasks.Count -ne 1) { throw '卡片忽略操作没有命中 GPT 消息' }
    $codexQueue = Invoke-RestMethod 'http://127.0.0.1:17891/api/tasks?view=queue&source=CODEX'
    if ($codexQueue.tasks.Count -ne 1) { throw '忽略操作影响了其他来源' }
    Shot '11a-card-action'
    Click 68 198
    Resize 860 540
    Click 270 134
    Shot '12-small-appearance'
    Click 395 134
    Shot '13-small-integrations'
    # 860 宽窗口的关闭按钮只应收起为悬浮球，不能销毁窗口或结束进程。
    Click 834 23
    Start-Sleep -Milliseconds 300
    if ((Rect).Right-(Rect).Left -ne 52) { throw '标题栏关闭没有收起为悬浮球' }
    Move-Pointer -10 -10
    Start-Sleep -Milliseconds 350
    Memory 'orb-after-panel'
    $ball = Rect
    if ($ball.Right-$ball.Left -ne 52) { throw '收起后不是圆球窗口' }
    # 捕获后按屏幕坐标拖动，松开不应打开主窗口。
    $point = (26 -shl 16) -bor 26
    Send 0x201 1 $point
    [HubUiVerify]::SetCursorPos($ball.Left-70, $ball.Top+100) | Out-Null
    Send 0x200 1 $point
    Send 0x202 0 $point
    Start-Sleep -Milliseconds 300
    if ((Rect).Right-(Rect).Left -ne 52) { throw '拖拽误触打开主窗口' }
    Shot '14-orb-after-drag'
    Write-Output 'PASS GUI screenshots, resize, hover, card action, detail scroll, drag-click separation and HTTP smoke'
} finally {
    # 普通 WM_CLOSE 现在按产品契约收起为悬浮球；测试退出走托盘专用退出消息。
    if ($script:window -ne [IntPtr]::Zero) { Send 0x800c }
    [HubUiVerify]::SetCursorPos($cursor.X, $cursor.Y) | Out-Null
    $process.WaitForExit(5000) | Out-Null
}
