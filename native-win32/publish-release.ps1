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
    $filled = "protocol=https`nhost=github.com`n`n" | git credential fill
    $line = $filled | Where-Object { $_ -like 'password=*' } | Select-Object -First 1
    if (-not $line) { throw '未找到 GitHub 凭据（git credential fill 没有返回密码）' }
    return $line.Substring('password='.Length)
}

$token = Get-GitHubToken
$api = 'https://api.github.com/repos/elaysia-feng/AI-Task-Hub'
$headers = @{
    Authorization = "Bearer $token"
    Accept = 'application/vnd.github+json'
    'X-GitHub-Api-Version' = '2022-11-28'
}

$payload = @{
    tag_name = $Tag
    name = $Title
    body = $notes
    draft = $false
    prerelease = $false
} | ConvertTo-Json -Depth 4

$release = Invoke-RestMethod -Method Post -Uri "$api/releases" -Headers $headers -Body $payload -ContentType 'application/json'
Write-Output "Release: $($release.html_url)"

foreach ($asset in $assets) {
    $fileName = [IO.Path]::GetFileName($asset)
    $uri = "https://uploads.github.com/repos/elaysia-feng/AI-Task-Hub/releases/$($release.id)/assets?name=$([uri]::EscapeDataString($fileName))"
    Invoke-RestMethod -Method Post -Uri $uri -Headers @{ Authorization = "Bearer $token" } -InFile $asset -ContentType 'application/octet-stream' | Out-Null
    Write-Output "Uploaded: $fileName"
}
Write-Output 'Done.'
