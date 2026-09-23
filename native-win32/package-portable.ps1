param(
    [string]$Version = '0.3.6',
    [string]$Executable = (Join-Path $PSScriptRoot 'dist/AI Task Hub Win32.next.exe')
)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+([-.][a-zA-Z0-9.-]+)?$') { throw '版本号格式无效' }
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$release = Join-Path $PSScriptRoot 'release'
$name = "AI-Task-Hub-Win32-x64-$Version"
$stage = Join-Path $release ($name + '-stage-' + [guid]::NewGuid().ToString('N'))
$package = Join-Path $stage $name
New-Item -ItemType Directory -Path $package -Force | Out-Null
Copy-Item -LiteralPath $Executable -Destination (Join-Path $package 'AI Task Hub Win32.exe')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'PORTABLE-README.md') -Destination (Join-Path $package 'README.md')
Copy-Item -LiteralPath (Join-Path $root 'shared/event_schema.json') -Destination $package
# 从源码白名单收集资源，不复制日常运行的 dist，避免夹带用户数据。
$resources = Join-Path $package 'resources'
New-Item -ItemType Directory -Path $resources -Force | Out-Null
foreach ($entry in @('anime-head.png','icon.ico','icon.png','tray.png','presets','themes')) {
    Copy-Item -LiteralPath (Join-Path $root "desktop/resources/$entry") -Destination $resources -Recurse
}
$adapterFiles = @('claude-code/claude_adapter.py','codex/notify_chain.py','codex/event_converter.py',
    'chatgpt-extension/background.js','chatgpt-extension/content.js','chatgpt-extension/manifest.json','chatgpt-extension/README.md')
foreach ($entry in $adapterFiles) {
    $destination = Join-Path $package "adapters/$entry"
    New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $root "adapters/$entry") -Destination $destination
}
$zip = Join-Path $release ($name + '.zip')
if (Test-Path -LiteralPath $zip) { throw '同名发布包已存在，不自动覆盖' }
Compress-Archive -LiteralPath $package -DestinationPath $zip -CompressionLevel Optimal
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive = [IO.Compression.ZipFile]::OpenRead($zip)
try {
    $invalid = @($archive.Entries | Where-Object { $_.FullName -match '(?i)(\.sqlite|\.ini$|\.log|\.next\.exe|session_titles|forward_target|\.env|\.bak)' })
    if ($invalid.Count) { throw '发布包包含禁止发布的运行期文件' }
    Write-Output ('Archive entries: ' + $archive.Entries.Count)
} finally { $archive.Dispose() }
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $release ($name + '.sha256')), "$hash  $name.zip`n", [Text.UTF8Encoding]::new($false))
Write-Output "Package: $zip"
Write-Output "SHA256: $hash"
