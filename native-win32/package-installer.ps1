param(
    [string]$Version = '0.3.7',
    [string]$Executable = (Join-Path $PSScriptRoot 'dist/AI Task Hub Win32.next.exe')
)
$ErrorActionPreference = 'Stop'
if ($Version -notmatch '^\d+\.\d+\.\d+([-.][a-zA-Z0-9.-]+)?$') { throw '版本号格式无效' }
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
. (Join-Path $PSScriptRoot 'package-common.ps1')

$compiler = Get-NsisCompiler
if (-not $compiler) { throw '未找到 NSIS（makensis）。请安装 NSIS，或把它加进 PATH。' }
if (-not (Test-Path -LiteralPath $Executable)) { throw "找不到待安装的 exe：$Executable" }

$release = Join-Path $PSScriptRoot 'release'
New-Item -ItemType Directory -Force -Path $release | Out-Null
$name = "AI-Task-Hub-Win32-x64-$Version"
$output = Join-Path $release ($name + '-Setup.exe')
if (Test-Path -LiteralPath $output) { throw '同名安装包已存在，不自动覆盖' }

# NSIS 按 ACP 读取没有 BOM 的脚本，含中文的 .nsi 会直接报 "Bad text encoding"。
# 脚本被编辑器改过而丢了 BOM 时，这里补回去。
$script = Join-Path $PSScriptRoot 'installer/AI-Task-Hub-Win32.nsi'
$scriptBytes = [IO.File]::ReadAllBytes($script)
if (-not ($scriptBytes.Length -ge 3 -and $scriptBytes[0] -eq 0xEF -and $scriptBytes[1] -eq 0xBB -and $scriptBytes[2] -eq 0xBF)) {
    [IO.File]::WriteAllBytes($script, [byte[]](0xEF, 0xBB, 0xBF) + $scriptBytes)
    Write-Output 'NSIS 脚本缺少 UTF-8 BOM，已补齐。'
}

# 暂存待安装文件：NSIS 从这里取文件，装到目标目录时是扁平结构
$stage = Join-Path $release ('.installer-stage-' + [guid]::NewGuid().ToString('N'))
try {
    New-Win32Payload -Root $root -Target $stage -Executable $Executable | Out-Null
    # NSIS 的 /D 定义值不能带引号，路径统一用正斜杠
    $stageDefine = ($stage -replace '\\', '/')
    $outputDefine = ($output -replace '\\', '/')
    & $compiler "/DVERSION=$Version" "/DSTAGE=$stageDefine" "/DOUTPUT=$outputDefine" $script
    if ($LASTEXITCODE -ne 0) { throw "NSIS 编译失败（exit $LASTEXITCODE）" }
} finally {
    if (Test-Path -LiteralPath $stage) { Remove-Item -LiteralPath $stage -Recurse -Force }
}

if (-not (Test-Path -LiteralPath $output)) { throw '安装包未生成' }
$hash = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $release ($name + '-Setup.sha256')), "$hash  $([IO.Path]::GetFileName($output))`n", [Text.UTF8Encoding]::new($false))
Write-Output "Installer: $output"
Write-Output "SHA256: $hash"
