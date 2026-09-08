Get-Process | Where-Object { $_.ProcessName -eq 'AI Task Hub Win32' } | Stop-Process -Force
Start-Sleep -Milliseconds 800
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
& (Join-Path $root 'native-win32/build-win32.ps1')
