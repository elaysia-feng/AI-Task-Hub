param(
    [string]$Version = '0.3.7',
    [string]$Tag = '',
    [string]$Title = '',
    [string]$NotesFile = '',
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+([-.][a-zA-Z0-9.-]+)?$') { throw '版本号格式无效' }
if (-not $Tag) { $Tag = "win32-v$Version" }
if (-not $Title) { $Title = "AI Task Hub Win32 v$Version" }

$releaseDir = Join-Path $PSScriptRoot 'release'
$name = "AI-Task-Hub-Win32-x64-$Version"
$assets = @(
    (Join-Path $releaseDir "$name.zip"),
    (Join-Path $releaseDir "$name-Setup.exe")
)
foreach ($asset in $assets) {
    if (-not (Test-Path -LiteralPath $asset)) { throw "缺少发布产物：$asset（先跑 package-portable.ps1 与 package-installer.ps1）" }
}

# 合并校验文件：对应发行页上的 SHA256SUMS.txt
$sums = Join-Path $releaseDir 'SHA256SUMS.txt'
$lines = foreach ($asset in $assets) {
    $hash = (Get-FileHash -LiteralPath $asset -Algorithm SHA256).Hash.ToLowerInvariant()
    "$hash  $([IO.Path]::GetFileName($asset))"
}
[IO.File]::WriteAllLines($sums, $lines, [Text.UTF8Encoding]::new($false))
$assets += $sums

$notes = "## 更新内容`n`n见提交记录。`n`n## 下载`n`n- 安装版：$name-Setup.exe（按用户安装，可从「设置 → 应用和功能」卸载）`n- 便携版：$name.zip（解压即用）`n`n同目录提供 SHA256SUMS.txt 校验文件。"
if ($NotesFile -and (Test-Path -LiteralPath $NotesFile)) {
    $notes = [IO.File]::ReadAllText((Resolve-Path $NotesFile), [Text.Encoding]::UTF8)
}

if ($DryRun) {
    Write-Output "Tag: $Tag"
    Write-Output "Title: $Title"
    foreach ($asset in $assets) {
        Write-Output ("Asset: {0} ({1} bytes)" -f [IO.Path]::GetFileName($asset), (Get-Item -LiteralPath $asset).Length)
    }
    Write-Output '--- notes ---'
    Write-Output $notes
    exit 0
}

# 凭据取自 Git Credential Manager（推送同一个仓库时已登录），不落盘、不打印。
function Get-GitHubToken {
    # PowerShell 7 管道给原生命令时默认带 UTF-8 BOM，git 会把首行读成
    # "﻿protocol" 而报 missing protocol field，这里先关掉 BOM。
    $OutputEncoding = [Text.UTF8Encoding]::new($false)
    $filled = "protocol=https`nhost=github.com`n`n" | git credential fill
    $line = $filled | Where-Object { $_ -like 'password=*' } | Select-Object -First 1
    if (-not $line) { throw '未找到 GitHub 凭据（git credential fill 没有返回密码）' }
    return $line.Substring('password='.Length)
}

$token = Get-GitHubToken
$api = 'https://api.github.com/repos/elaysia-feng/AI-Task-Hub'
$uploads = 'https://uploads.github.com/repos/elaysia-feng/AI-Task-Hub'
$auth = "Authorization: Bearer $token"
$accept = 'Accept: application/vnd.github+json'
$work = Join-Path ([IO.Path]::GetTempPath()) ('aihub-release-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $work -Force | Out-Null

try {
    # 大文件上传走 curl：本机代理会让 .NET 的 HttpClient 偶发 "unexpected EOF"，
    # curl 的 --retry 对这种瞬断更耐打。
    $curl = (Get-Command curl.exe -ErrorAction SilentlyContinue).Source
    if (-not $curl) { throw '未找到 curl.exe' }

    # 1) 找已有发行（重跑时复用），没有就新建
    $releaseJson = Join-Path $work 'release.json'
    & $curl -sS --retry 5 --retry-delay 3 --retry-all-errors --max-time 120 `
        -H $auth -H $accept "$api/releases/tags/$Tag" -o $releaseJson -w '%{http_code}' | Out-Null
    $release = $null
    if (Test-Path $releaseJson) {
        $parsed = Get-Content $releaseJson -Raw | ConvertFrom-Json
        if ($parsed.id) { $release = $parsed }
    }
    if (-not $release) {
        $payloadFile = Join-Path $work 'payload.json'
        $payload = @{
            tag_name = $Tag
            name = $Title
            body = $notes
            draft = $false
            prerelease = $false
        } | ConvertTo-Json -Depth 4
        [IO.File]::WriteAllText($payloadFile, $payload, [Text.UTF8Encoding]::new($false))
        & $curl -sS --retry 5 --retry-delay 3 --retry-all-errors --max-time 120 -X POST `
            -H $auth -H $accept -H 'X-GitHub-Api-Version: 2022-11-28' -H 'Content-Type: application/json' `
            --data-binary "@$payloadFile" -o $releaseJson "$api/releases" | Out-Null
        $release = Get-Content $releaseJson -Raw | ConvertFrom-Json
        if (-not $release.id) { throw "创建发行失败：$(Get-Content $releaseJson -Raw)" }
    }
    Write-Output "Release: $($release.html_url)"

    # 2) 上传资产（已存在的同名资产先删掉再传，便于重跑）
    $existing = @{}
    if ($release.assets) { foreach ($item in $release.assets) { $existing[$item.name] = $item.id } }
    foreach ($asset in $assets) {
        $fileName = [IO.Path]::GetFileName($asset)
        if ($existing.ContainsKey($fileName)) {
            & $curl -sS --retry 5 --retry-delay 2 --retry-all-errors --max-time 60 -X DELETE `
                -H $auth "$api/releases/assets/$($existing[$fileName])" -o NUL | Out-Null
            Write-Output "Removed old asset: $fileName"
        }
        $uri = "$uploads/releases/$($release.id)/assets?name=$([uri]::EscapeDataString($fileName))"
        $out = Join-Path $work 'upload.json'
        & $curl -sS --retry 5 --retry-delay 5 --retry-all-errors --max-time 1800 -X POST `
            -H $auth -H 'Content-Type: application/octet-stream' `
            --data-binary "@$asset" -o $out $uri | Out-Null
        $uploaded = Get-Content $out -Raw | ConvertFrom-Json
        if (-not $uploaded.id) { throw "上传 $fileName 失败：$(Get-Content $out -Raw)" }
        Write-Output ("Uploaded: {0} ({1} bytes)" -f $uploaded.name, $uploaded.size)
    }
    Write-Output 'Done.'
} finally {
    if (Test-Path -LiteralPath $work) { Remove-Item -LiteralPath $work -Recurse -Force }
}
