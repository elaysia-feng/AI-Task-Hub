param([switch]$StageOnly)
$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$source = Join-Path $root 'native-win32'
$obj = Join-Path $source 'obj'
$dist = Join-Path $source 'dist'
$compiler = (Get-Command g++.exe -ErrorAction SilentlyContinue).Source
$cCompiler = (Get-Command gcc.exe -ErrorAction SilentlyContinue).Source
if ([string]::IsNullOrWhiteSpace($compiler)) {
    $compiler = 'F:\vscode_environment\c\MinGW\winlibs-x86_64-posix-seh-gcc-15.1.0-mingw-w64ucrt-13.0.0-r3\mingw64\bin\g++.exe'
    $cCompiler = 'F:\vscode_environment\c\MinGW\winlibs-x86_64-posix-seh-gcc-15.1.0-mingw-w64ucrt-13.0.0-r3\mingw64\bin\gcc.exe'
}

New-Item -ItemType Directory -Force -Path $obj, $dist | Out-Null
$defines = @('-DUNICODE', '-D_UNICODE', '-DWIN32_LEAN_AND_MEAN', '-DNOMINMAX', '-D_WIN32_WINNT=0x0A00',
    '-DSQLITE_THREADSAFE=1', '-DSQLITE_DEFAULT_MEMSTATUS=0', '-DSQLITE_OMIT_LOAD_EXTENSION', '-DSQLITE_OMIT_DEPRECATED')
$includes = @('-I', (Join-Path $source 'src'), '-I', (Join-Path $source 'third_party\sqlite-amalgamation-3450200'))
$common = @('-std=c++17', '-O2') + $defines + $includes

& $compiler @common '-municode' '-c' (Join-Path $source 'src\main.cpp') '-o' (Join-Path $obj 'main.o')
if ($LASTEXITCODE -ne 0) { throw "main.cpp 编译失败（exit $LASTEXITCODE）" }
& $compiler @common '-c' (Join-Path $source 'src\json_lite.cpp') '-o' (Join-Path $obj 'json_lite.o')
if ($LASTEXITCODE -ne 0) { throw "json_lite.cpp 编译失败（exit $LASTEXITCODE）" }
& $compiler @common '-c' (Join-Path $source 'src\task_store.cpp') '-o' (Join-Path $obj 'task_store.o')
if ($LASTEXITCODE -ne 0) { throw "task_store.cpp 编译失败（exit $LASTEXITCODE）" }
& $compiler @common '-c' (Join-Path $source 'src\http_server.cpp') '-o' (Join-Path $obj 'http_server.o')
if ($LASTEXITCODE -ne 0) { throw "http_server.cpp 编译失败（exit $LASTEXITCODE）" }
& $compiler @common '-c' (Join-Path $source 'src\integration_manager.cpp') '-o' (Join-Path $obj 'integration_manager.o')
if ($LASTEXITCODE -ne 0) { throw "integration_manager.cpp 编译失败（exit $LASTEXITCODE）" }
& $cCompiler '-O2' $defines[4..8] '-DSQLITE_OMIT_DEPRECATED' '-I' (Join-Path $source 'third_party\sqlite-amalgamation-3450200') '-c' (Join-Path $source 'third_party\sqlite-amalgamation-3450200\sqlite3.c') '-o' (Join-Path $obj 'sqlite3.o')
if ($LASTEXITCODE -ne 0) { throw "sqlite3.c 编译失败（exit $LASTEXITCODE）" }

$output = Join-Path $dist 'AI Task Hub Win32.next.exe'
$objects = @((Join-Path $obj 'main.o'), (Join-Path $obj 'json_lite.o'), (Join-Path $obj 'sqlite3.o'), (Join-Path $obj 'task_store.o'), (Join-Path $obj 'http_server.o'), (Join-Path $obj 'integration_manager.o'))
# 明确使用 Windows GUI 子系统；否则双击 exe 会被当成控制台程序并自动打开 Terminal。
& $compiler '-mwindows' '-municode' '-static' '-static-libgcc' '-static-libstdc++' '-s' @objects '-o' $output '-ld2d1' '-ldwrite' '-lwindowscodecs' '-ldwmapi' '-lshlwapi' '-lshell32' '-lole32' '-luuid' '-lcomctl32' '-lcomdlg32' '-luser32' '-lgdi32' '-lws2_32'
if ($LASTEXITCODE -ne 0) { throw "链接失败（exit $LASTEXITCODE）" }
if (-not $StageOnly) {
    $published = Join-Path $dist 'AI Task Hub Win32.exe'
    Move-Item -LiteralPath $output -Destination $published -Force
    $output = $published
}

$resources = Join-Path $dist 'resources'
New-Item -ItemType Directory -Force -Path $resources | Out-Null
Copy-Item -Path (Join-Path $root 'desktop\resources\*') -Destination $resources -Recurse -Force
Copy-Item -LiteralPath (Join-Path $root 'shared\event_schema.json') -Destination $dist -Force
$adapterOutput = Join-Path $dist 'adapters'
New-Item -ItemType Directory -Force -Path (Join-Path $adapterOutput 'claude-code'), (Join-Path $adapterOutput 'codex'), (Join-Path $adapterOutput 'chatgpt-extension') | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'adapters\claude-code\claude_adapter.py') -Destination (Join-Path $adapterOutput 'claude-code') -Force
# 旧构建曾把个人会话标题缓存装入发布包，移出发布目录并保留可恢复副本。
$staleCache = Join-Path $adapterOutput 'claude-code\session_titles.json'
if (Test-Path -LiteralPath $staleCache -PathType Leaf) {
    Move-Item -LiteralPath $staleCache -Destination (Join-Path $obj ('session_titles.packaging-backup-' + [guid]::NewGuid().ToString('N') + '.json'))
}
Copy-Item -LiteralPath (Join-Path $root 'adapters\codex\notify_chain.py'), (Join-Path $root 'adapters\codex\event_converter.py') -Destination (Join-Path $adapterOutput 'codex') -Force
Copy-Item -LiteralPath (Join-Path $root 'adapters\chatgpt-extension\background.js'), (Join-Path $root 'adapters\chatgpt-extension\content.js'), (Join-Path $root 'adapters\chatgpt-extension\manifest.json'), (Join-Path $root 'adapters\chatgpt-extension\README.md') -Destination (Join-Path $adapterOutput 'chatgpt-extension') -Force
Write-Output "Built: $output"
