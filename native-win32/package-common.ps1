# Win32 发布包共用逻辑：整理待发布文件、定位 NSIS 编译器。
# package-portable.ps1（便携版 ZIP）与 package-installer.ps1（NSIS 安装程序）共用，
# 避免两条发布流打包出内容不一致的产物。

# 适配器白名单：从源码收集，不复制 dist，避免夹带用户数据与运行时缓存。
$script:Win32AdapterFiles = @(
    'claude-code/claude_adapter.py',
    'codex/notify_chain.py', 'codex/event_converter.py', 'codex/prompt_hook.py',
    'chatgpt-extension/background.js', 'chatgpt-extension/content.js',
    'chatgpt-extension/manifest.json', 'chatgpt-extension/README.md',
    'zcode/zcode_adapter.py',
    'minimax-code/minimax_adapter.py', 'minimax-code/plugin.json',
    'minimax-code/hooks.json', 'minimax-code/icon.png'
)

function New-Win32Payload {
    <#
      把程序本体复制到 $Target（扁平结构，NSIS 安装程序直接使用该目录）。
      .SYNOPSIS
        整理 Win32 版待发布文件。
    #>
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Target,
        [Parameter(Mandatory = $true)][string]$Executable
    )
    New-Item -ItemType Directory -Path $Target -Force | Out-Null
    Copy-Item -LiteralPath $Executable -Destination (Join-Path $Target 'AI Task Hub Win32.exe')
    Copy-Item -LiteralPath (Join-Path $Root 'native-win32/PORTABLE-README.md') -Destination (Join-Path $Target 'README.md')
    Copy-Item -LiteralPath (Join-Path $Root 'shared/event_schema.json') -Destination $Target

    $resources = Join-Path $Target 'resources'
    New-Item -ItemType Directory -Path $resources -Force | Out-Null
    foreach ($entry in @('anime-head.png', 'icon.ico', 'icon.png', 'tray.png', 'presets', 'themes')) {
        Copy-Item -LiteralPath (Join-Path $Root "desktop/resources/$entry") -Destination $resources -Recurse
    }

    foreach ($entry in $script:Win32AdapterFiles) {
        $destination = Join-Path $Target "adapters/$entry"
        New-Item -ItemType Directory -Path (Split-Path $destination) -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $Root "adapters/$entry") -Destination $destination
    }
    return $Target
}

function Get-NsisCompiler {
    <#
      定位 makensis：先找 PATH，再找常见安装目录（与 desktop/scripts/ensure-nsis.mjs 同口径）。
    #>
    $onPath = (Get-Command makensis.exe -ErrorAction SilentlyContinue).Source
    if ($onPath) { return $onPath }
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA 'Programs/NSIS/makensis.exe'),
        'E:/develop/NSIS/makensis.exe',
        'D:/develop/NSIS/makensis.exe',
        (Join-Path ${env:ProgramFiles(x86)} 'NSIS/makensis.exe'),
        (Join-Path $env:ProgramFiles 'NSIS/makensis.exe')
    )
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { return $candidate }
    }
    return $null
}
