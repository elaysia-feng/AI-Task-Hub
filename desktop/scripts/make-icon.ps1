# 生成应用图标：石墨圆角方块 + 四向汇聚 hub 图形
# 输出：icon.png / tray.png / icon.ico（多尺寸，供 electron-builder 与 PyInstaller）
# 用法：pwsh scripts/make-icon.ps1
param(
    [string]$OutDir = (Join-Path $PSScriptRoot '..\resources')
)

$ErrorActionPreference = 'Stop'
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
        [System.Drawing.Color]::FromArgb(255, 49, 58, 61),
        [System.Drawing.Color]::FromArgb(255, 23, 27, 29),
        55
    )
    $inset = [Math]::Max(0, $size * 0.025)
    $radius = [Math]::Max(1, $size * 0.24)
    $roundPath = New-RoundedRectPath $inset $inset ($size - $inset * 2) ($size - $inset * 2) $radius
    $g.FillPath($brush, $roundPath)

    $borderPen = New-Object System.Drawing.Pen(
        [System.Drawing.Color]::FromArgb(72, 255, 255, 255),
        [Math]::Max(1, $size * 0.008)
    )
    $g.DrawPath($borderPen, $roundPath)

    $ivory = [System.Drawing.Color]::FromArgb(255, 247, 243, 235)
    $coral = [System.Drawing.Color]::FromArgb(255, 223, 118, 84)
    $ink = [System.Drawing.Color]::FromArgb(255, 32, 38, 41)
    $cx = $size / 2
    $cy = $size / 2
    $outer = @(
        @(($size * 0.29), ($size * 0.29)),
        @(($size * 0.71), ($size * 0.29)),
        @(($size * 0.29), ($size * 0.71)),
        @(($size * 0.71), ($size * 0.71))
    )

    $pen = New-Object System.Drawing.Pen($ivory, [Math]::Max(1, $size * 0.047))
    $pen.StartCap = [System.Drawing.Drawing2D.LineCap]::Round
    $pen.EndCap = [System.Drawing.Drawing2D.LineCap]::Round
    foreach ($pt in $outer) {
        $g.DrawLine($pen, $cx, $cy, $pt[0], $pt[1])
    }

    $nodeR = [Math]::Max(1, $size * 0.055)
    $nodeBrush = New-Object System.Drawing.SolidBrush($ivory)
    foreach ($pt in $outer) {
        $g.FillEllipse($nodeBrush, $pt[0] - $nodeR, $pt[1] - $nodeR, $nodeR * 2, $nodeR * 2)
    }

    $centerR = [Math]::Max(1.5, $size * 0.115)
    $centerBrush = New-Object System.Drawing.SolidBrush($coral)
    $g.FillEllipse($centerBrush, $cx - $centerR, $cy - $centerR, $centerR * 2, $centerR * 2)

    $coreR = [Math]::Max(0.7, $size * 0.038)
    $coreBrush = New-Object System.Drawing.SolidBrush($ink)
    $g.FillEllipse($coreBrush, $cx - $coreR, $cy - $coreR, $coreR * 2, $coreR * 2)

    $g.Dispose()
    $brush.Dispose()
    $borderPen.Dispose()
    $pen.Dispose()
    $nodeBrush.Dispose()
    $centerBrush.Dispose()
    $coreBrush.Dispose()
    $roundPath.Dispose()
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
