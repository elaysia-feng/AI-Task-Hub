# =============================================================================
# make-icon.ps1 —— 从单一源图生成应用全套图标
#
# 【单一源图模型】
#   唯一源图：desktop/resources/anime-head.png（256×256 正方形，樱花粉二次元女头）
#   更换图标 = 替换 anime-head.png，然后重跑本脚本即可再生成全套：
#     - desktop/resources/icon.png   256×256
#     - desktop/resources/tray.png    32×32 圆形（透明四角，供系统托盘）
#     - desktop/resources/icon.ico    16/24/32/48/64/128/256 多尺寸（PNG 帧，32bppArgb）
#     - packaging/app.ico             icon.ico 副本（供 PyInstaller 后端 exe）
#   用法：pwsh desktop/scripts/make-icon.ps1
#   本脚本不依赖当前工作目录：路径一律基于 $PSScriptRoot 解析，可任意 cwd 运行。
# =============================================================================

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root      = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$resources = Join-Path $root 'desktop\resources'
$packaging = Join-Path $root 'packaging'
$sourcePng = Join-Path $resources 'anime-head.png'

if (-not (Test-Path -LiteralPath $sourcePng)) {
    Write-Host "错误：缺少源图：$sourcePng" -ForegroundColor Red
    Write-Host '请先重新生成 anime-head.png（256×256 樱花粉二次元女头）后再运行本脚本。' -ForegroundColor Red
    exit 1
}

# 从源图缩放出目标尺寸位图（32bppArgb，避免 PNG tRNS 导致 electron-builder 转 ico 失败/回退）
function New-HubBitmap([System.Drawing.Bitmap]$src, [int]$size) {
    $bmp = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::HighQuality
    $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.DrawImage($src, 0, 0, $size, $size)
    $g.Dispose()
    return $bmp
}

# 圆形托盘图标：源图缩放后按内切圆做 alpha 掩码，四角透明
function New-TrayBitmap([System.Drawing.Bitmap]$src, [int]$size) {
    $bmp = New-HubBitmap $src $size

    $mask = New-Object System.Drawing.Bitmap($size, $size, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($mask)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $g.Clear([System.Drawing.Color]::Transparent)
    $g.FillEllipse([System.Drawing.Brushes]::White, 0, 0, $size, $size)
    $g.Dispose()

    for ($y = 0; $y -lt $size; $y++) {
        for ($x = 0; $x -lt $size; $x++) {
            $c = $bmp.GetPixel($x, $y)
            $m = $mask.GetPixel($x, $y)
            $a = [int]($c.A * $m.A / 255)
            $bmp.SetPixel($x, $y, [System.Drawing.Color]::FromArgb($a, $c.R, $c.G, $c.B))
        }
    }
    $mask.Dispose()
    return $bmp
}

function Save-Png([System.Drawing.Bitmap]$bmp, [string]$path) {
    $bmp.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
}

function Save-Ico([System.Drawing.Bitmap]$src, [string]$icoPath, [int[]]$sizes) {
    # 手工写 ICO：多帧 PNG 嵌入（Vista+），避免 System.Drawing Icon 质量差
    $frames = @()
    foreach ($s in $sizes) {
        $bmp = New-HubBitmap $src $s
        $ms = New-Object System.IO.MemoryStream
        $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
        $frames += ,@{ Size = $s; Bytes = $ms.ToArray() }
        $ms.Dispose()
        $bmp.Dispose()
    }

    $fs = [System.IO.File]::Create($icoPath)
    $bw = New-Object System.IO.BinaryWriter($fs)
    # ICONDIR
    $bw.Write([uint16]0)            # reserved
    $bw.Write([uint16]1)            # type = icon
    $bw.Write([uint16]$frames.Count)

    $offset = 6 + (16 * $frames.Count)
    foreach ($f in $frames) {
        $s = $f.Size
        $len = $f.Bytes.Length
        $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))  # width
        $bw.Write([byte]($(if ($s -ge 256) { 0 } else { $s })))  # height
        $bw.Write([byte]0)    # color count
        $bw.Write([byte]0)    # reserved
        $bw.Write([uint16]1)  # planes
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

New-Item -ItemType Directory -Force -Path $resources | Out-Null
New-Item -ItemType Directory -Force -Path $packaging | Out-Null

$src = New-Object System.Drawing.Bitmap($sourcePng)

$iconPng = Join-Path $resources 'icon.png'
$trayPng = Join-Path $resources 'tray.png'
$iconIco = Join-Path $resources 'icon.ico'

$big = New-HubBitmap $src 256
Save-Png $big $iconPng
$big.Dispose()

$tray = New-TrayBitmap $src 32
Save-Png $tray $trayPng
$tray.Dispose()

Save-Ico $src $iconIco @(16, 24, 32, 48, 64, 128, 256)

$src.Dispose()

# 同步一份到 packaging，供 PyInstaller 后端 exe 使用
Copy-Item $iconIco (Join-Path $packaging 'app.ico') -Force

Write-Output 'icons written:'
Write-Output "  $iconPng"
Write-Output "  $trayPng"
Write-Output "  $iconIco"
Write-Output "  $(Join-Path $packaging 'app.ico')"
