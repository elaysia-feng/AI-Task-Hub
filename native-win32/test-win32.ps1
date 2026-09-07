$ErrorActionPreference = 'Stop'
$compiler = (Get-Command g++.exe -ErrorAction SilentlyContinue).Source
if (-not $compiler) {
    $compiler = 'F:\vscode_environment\c\MinGW\winlibs-x86_64-posix-seh-gcc-15.1.0-mingw-w64ucrt-13.0.0-r3\mingw64\bin\g++.exe'
}
$obj = Join-Path $PSScriptRoot 'obj'
if (-not (Test-Path -LiteralPath (Join-Path $obj 'sqlite3.o'))) { throw '请先运行 build-win32.ps1 -StageOnly' }
$output = Join-Path $obj 'native-tests.exe'
& $compiler '-std=c++17' '-O2' '-municode' '-DUNICODE' '-D_UNICODE' '-DNOMINMAX' '-D_WIN32_WINNT=0x0A00' '-static-libgcc' '-static-libstdc++' '-I' (Join-Path $PSScriptRoot 'src') '-I' (Join-Path $PSScriptRoot 'third_party/sqlite-amalgamation-3450200') (Join-Path $PSScriptRoot 'tests/native_tests.cpp') (Join-Path $PSScriptRoot 'src/json_lite.cpp') (Join-Path $PSScriptRoot 'src/task_store.cpp') (Join-Path $obj 'sqlite3.o') '-o' $output '-lshell32' '-lole32' '-luuid'
if ($LASTEXITCODE -ne 0) { throw '测试程序编译失败' }
$fixture = Join-Path $obj ('test-fixtures-' + [guid]::NewGuid().ToString('N'))
& $output $fixture
if ($LASTEXITCODE -ne 0) { throw "回归测试失败；隔离数据保留在 $fixture" }
Write-Output "Fixtures: $fixture"
