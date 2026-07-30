# 生成应用图标：渐变圆角方块 + 白色 hub 图形（三个节点连线）
# 用法：pwsh scripts/make-icon.ps1  → 输出 resources/icon.png (256px) 与 resources/tray.png (32px)
param(
    [string]$OutDir = (Join-Path $PSScriptRoot '..\resources')
)

Add-Type -AssemblyName System.Drawing

function New-RoundedRectPath([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $path.AddArc($x, $y, $d, $d, 180, 90)
    $path.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $path.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90)
    $path.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    return $path
}

function Draw-HubIcon([int]$size, [string]$path) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias

    $rect = New-Object System.Drawing.Rectangle(0, 0, $size, $size)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $rect,
        [System.Drawing.Color]::FromArgb(255, 99, 102, 241),   # #6366f1
        [System.Drawing.Color]::FromArgb(255, 168, 85, 247),   # #a855f7
        45
    )
    $radius = $size * 0.22
    $roundPath = New-RoundedRectPath 0 0 $size $size $radius
    $g.FillPath($brush, $roundPath)

    # hub 图形：顶点小圆 + 底部两个圆 + 连线
    $white = [System.Drawing.Color]::White
    $nodeR = $size * 0.085
    $cx = $size / 2
    $topY = $size * 0.30
    $leftX = $size * 0.30; $botY = $size * 0.68
    $rightX = $size * 0.70

    $pen = New-Object System.Drawing.Pen($white, ($size * 0.05))
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawLine($pen, $cx, $topY, $leftX, $botY)
    $g.DrawLine($pen, $cx, $topY, $rightX, $botY)
    $g.DrawLine($pen, $leftX, $botY, $rightX, $botY)

    $nodeBrush = New-Object System.Drawing.SolidBrush($white)
    foreach ($pt in @(@($cx, $topY), @($leftX, $botY), @($rightX, $botY))) {
        $g.FillEllipse($nodeBrush, $pt[0] - $nodeR, $pt[1] - $nodeR, $nodeR * 2, $nodeR * 2)
    }

    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose()
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Draw-HubIcon 256 (Join-Path $OutDir 'icon.png')
Draw-HubIcon 32  (Join-Path $OutDir 'tray.png')
Write-Output "icons written to $OutDir"
