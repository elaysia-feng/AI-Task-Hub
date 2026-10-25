param(
    [string]$Version = '0.3.7',
    [string]$Executable = (Join-Path $PSScriptRoot 'dist/AI Task Hub Win32.next.exe')
)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+([-.][a-zA-Z0-9.-]+)?$') { throw '版本号格式无效' }
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot 'package-common.ps1')

$release = Join-Path $PSScriptRoot 'release'
$name = "AI-Task-Hub-Win32-x64-$Version"
$stage = Join-Path $release ($name + '-stage-' + [guid]::NewGuid().ToString('N'))
# ZIP 内保留一层同名目录，解压后直接得到一个干净的目录
$package = Join-Path $stage $name
New-Win32Payload -Root $root -Target $package -Executable $Executable | Out-Null

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
if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
$hash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $release ($name + '.sha256')), "$hash  $name.zip`n", [Text.UTF8Encoding]::new($false))
Write-Output "Package: $zip"
Write-Output "SHA256: $hash"
