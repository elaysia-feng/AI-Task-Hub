[CmdletBinding()]
param(
    [string]$QtRoot = "E:\QT\6.8.3\mingw_64",
    [string]$OutputDirectory = "",
    [string]$BuildExecutable = ""
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$nativeRoot = Join-Path $repoRoot "native"
$buildDirectory = Join-Path $nativeRoot "build"
$exePath = ""
if ([string]::IsNullOrWhiteSpace($BuildExecutable)) {
    $exePath = Join-Path $buildDirectory "ai-task-hub-native.exe"
} else {
    $exePath = [System.IO.Path]::GetFullPath($BuildExecutable)
}
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $nativeRoot "dist"
}
$outputPath = [System.IO.Path]::GetFullPath($OutputDirectory)

if (-not (Test-Path -LiteralPath (Join-Path $QtRoot "bin\windeployqt.exe"))) {
    throw "未找到 windeployqt：$QtRoot。请安装 Qt 6.8.3 MinGW，或使用 -QtRoot 指定路径。"
}

if ([string]::IsNullOrWhiteSpace($BuildExecutable)) {
    cmake -S $nativeRoot -B $buildDirectory -G Ninja -DCMAKE_PREFIX_PATH=$QtRoot
    cmake --build $buildDirectory --parallel 2
}
if (-not (Test-Path -LiteralPath $exePath)) {
    throw "原生可执行文件不存在：$exePath；可先构建，或通过 -BuildExecutable 指定已验证的 EXE。"
}

# 只清理 native 下的精确产物目录，避免误删仓库或用户数据。
$nativePrefix = [System.IO.Path]::GetFullPath($nativeRoot) + [System.IO.Path]::DirectorySeparatorChar
if (-not $outputPath.StartsWith($nativePrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "OutputDirectory 必须位于 native 目录内：$outputPath"
}
if (Test-Path -LiteralPath $outputPath) {
    Remove-Item -LiteralPath $outputPath -Recurse -Force
}
New-Item -ItemType Directory -Path $outputPath | Out-Null
$deployedExe = Join-Path $outputPath "AI Task Hub Native.exe"
Copy-Item -LiteralPath $exePath -Destination $deployedExe

$windeployqt = Join-Path $QtRoot "bin\windeployqt.exe"
& $windeployqt --release --no-translations --no-system-d3d-compiler --qmldir (Join-Path $nativeRoot "qml") $deployedExe
Copy-Item -LiteralPath (Join-Path $nativeRoot "README.md") -Destination (Join-Path $outputPath "README-native.md")
Copy-Item -LiteralPath (Join-Path $repoRoot "shared\event_schema.json") -Destination (Join-Path $outputPath "event_schema.json")

# 内置角色头像和 Light/Dark 壁纸按原目录布局带入，QML 会在缺失时自动回退到渐变背景。
$resourceOutput = Join-Path $outputPath "resources"
New-Item -ItemType Directory -Path $resourceOutput -Force | Out-Null
Get-ChildItem -LiteralPath (Join-Path $repoRoot "desktop\resources") -File |
    Where-Object { $_.Extension -in ".png", ".jpg", ".jpeg", ".webp", ".bmp", ".ico" } |
    ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $resourceOutput $_.Name)
    }
Get-ChildItem -LiteralPath (Join-Path $repoRoot "desktop\resources\presets") -Recurse -File |
    Where-Object { $_.Extension -in ".png", ".jpg", ".jpeg", ".webp", ".bmp" } |
    ForEach-Object {
        $relative = $_.FullName.Substring((Join-Path $repoRoot "desktop\resources").Length).TrimStart("\", "/")
        $destination = Join-Path $resourceOutput $relative
        New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $destination
    }
Get-ChildItem -LiteralPath (Join-Path $repoRoot "desktop\resources\themes") -Recurse -File |
    Where-Object { $_.Extension -in ".png", ".jpg", ".jpeg", ".webp", ".bmp" } |
    ForEach-Object {
        $relative = $_.FullName.Substring((Join-Path $repoRoot "desktop\resources").Length).TrimStart("\", "/")
        $destination = Join-Path $resourceOutput $relative
        New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $destination
    }

# 适配器是轻量脚本，不参与 native 进程；把协议适配所需的源码和 manifest 一并带上，
# 用户可以直接从分发目录加载 ChatGPT 扩展，或把 Claude/Codex hook 指向同一端口。
$adapterOutput = Join-Path $outputPath "adapters"
Get-ChildItem -LiteralPath (Join-Path $repoRoot "adapters") -Recurse -File |
    Where-Object { $_.Extension -in ".py", ".js", ".json", ".md", ".html" } |
    ForEach-Object {
        $relative = $_.FullName.Substring((Join-Path $repoRoot "adapters").Length).TrimStart("\", "/")
        $destination = Join-Path $adapterOutput $relative
        New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $destination
    }

Write-Host "原生分发目录已生成：$outputPath"
Write-Host "运行：$deployedExe"
