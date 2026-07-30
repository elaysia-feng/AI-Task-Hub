# 生成应用图标：渐变圆角方块 + 白色 hub 图形
# 输出：icon.png / tray.png / icon.ico（多尺寸，供 electron-builder 与 PyInstaller）
# 用法：pwsh scripts/make-icon.ps1
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

function New-HubBitmap([int]$size) {
    # 32bppArgb，避免 PNG tRNS 导致 electron-builder 转 ico 失败/回退
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)

    $rect = New-Object System.Drawing.Rectangle(0, 0, $size, $size)
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush(
        $rect,
        [System.Drawing.Color]::FromArgb(255, 99, 102, 241),
        [System.Drawing.Color]::FromArgb(255, 168, 85, 247),
        45
    )
    $radius = [Math]::Max(1, $size * 0.22)
    $roundPath = New-RoundedRectPath 0 0 $size $size $radius
    $g.FillPath($brush, $roundPath)

    $white = [System.Drawing.Color]::White
    $nodeR = [Math]::Max(1, $size * 0.085)
    $cx = $size / 2
    $topY = $size * 0.30
    $leftX = $size * 0.30; $botY = $size * 0.68
    $rightX = $size * 0.70

    $pen = New-Object System.Drawing.Pen($white, [Math]::Max(1, $size * 0.05))
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    $g.DrawLine($pen, $cx, $topY, $leftX, $botY)
    $g.DrawLine($pen, $cx, $topY, $rightX, $botY)
    $g.DrawLine($pen, $leftX, $botY, $rightX, $botY)

    $nodeBrush = New-Object System.Drawing.SolidBrush($white)
    foreach ($pt in @(@($cx, $topY), @($leftX, $botY), @($rightX, $botY))) {
        $g.FillEllipse($nodeBrush, $pt[0] - $nodeR, $pt[1] - $nodeR, $nodeR * 2, $nodeR * 2)
    }

    $g.Dispose()
    $brush.Dispose()
    $pen.Dispose()
    $nodeBrush.Dispose()
    return $bmp
}

function Save-Png([System.Drawing.Bitmap]$bmp, [string]$path) {
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
}

function Save-Ico([string]$icoPath, [int[]]$sizes) {
    # 手工写 ICO：多帧 PNG 嵌入（Vista+），避免 System.Drawing Icon 质量差
    $frames = @()
    foreach ($s in $sizes) {
        $bmp = New-HubBitmap $s
        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $frames += ,@{ Size = $s; Bytes = $ms.ToArray() }
        $ms.Dispose()
        $bmp.Dispose()
    }

    $fs = [System.IO.File]::Create($icoPath)
    $bw = New-Object System.IO.BinaryWriter($fs)
    # ICONDIR
    $bw.Write([uint16]0)           # reserved
    $bw.Write([uint16]1)           # type = icon
    $bw.Write([uint16]$frames.Count)

    $offset = 6 + (16 * $frames.Count)
    foreach ($f in $frames) {
        $s = $f.Size
        $len = $f.Bytes.Length
        $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s }))) # width
        $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s }))) # height
        $bw.Write([byte]0)   # color count
        $bw.Write([byte]0)   # reserved
        $bw.Write([uint16]1) # planes
        $bw.Write([uint16]32) # bit count
        $bw.Write([uint32]$len)
        $bw.Write([uint32]$offset)
        $offset += $len
    }
    foreach ($f in $frames) {
        $bw.Write($f.Bytes)
    }
    $bw.Flush()
    $bw.Dispose()
    $fs.Dispose()
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$iconPng = Join-Path $OutDir 'icon.png'
$trayPng = Join-Path $OutDir 'tray.png'
$iconIco = Join-Path $OutDir 'icon.ico'

$big = New-HubBitmap 256
Save-Png $big $iconPng
$big.Dispose()

$tray = New-HubBitmap 32
Save-Png $tray $trayPng
$tray.Dispose()

Save-Ico $iconIco @(16, 24, 32, 48, 64, 128, 256)

# 同步一份到 packaging，供 PyInstaller 后端 exe 使用
$packagingDir = Join-Path $PSScriptRoot '..\..\packaging'
New-Item -ItemType Directory -Force -Path $packagingDir | Out-Null
Copy-Item $iconIco (Join-Path $packagingDir 'app.ico') -Force

Write-Output "icons written:"
Write-Output "  $iconPng"
Write-Output "  $trayPng"
Write-Output "  $iconIco"
Write-Output "  $(Join-Path $packagingDir 'app.ico')"
