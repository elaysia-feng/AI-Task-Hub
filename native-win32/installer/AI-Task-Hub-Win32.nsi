; AI Task Hub · Win32 原生版安装程序（NSIS）
;
; 与便携版并存：便携版解压即用，安装版提供开始菜单/桌面快捷方式与卸载项。
; 按用户安装（不需要管理员权限），默认装到 %LOCALAPPDATA%\Programs，
; 避免装进 Program Files 后程序无法在 exe 同目录写 data.sqlite。
;
; 编译参数（由 package-installer.ps1 传入）：
;   /DVERSION=0.3.7        版本号
;   /DSTAGE=<payload 目录>  待安装文件的暂存目录

Unicode true

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef STAGE
  !define STAGE "..\release\.stage"
!endif
; 输出路径由构建脚本传入（NSIS 的相对路径以脚本所在目录为基准，不便放进 release 目录）
!ifndef OUTPUT
  !define OUTPUT "AI-Task-Hub-Win32-Setup.exe"
!endif

!define APP_NAME "AI Task Hub Win32"
!define APP_EXE "AI Task Hub Win32.exe"
!define APP_PUBLISHER "AI Task Hub"
!define APP_REGKEY "Software\AI Task Hub"
!define APP_UNINSTKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\AI_Task_Hub_Win32"

Name "${APP_NAME} ${VERSION}"
OutFile "${OUTPUT}"
InstallDir "$LOCALAPPDATA\Programs\AI Task Hub Win32"
InstallDirRegKey HKCU "${APP_REGKEY}" "InstallDir"
RequestExecutionLevel user
ShowInstDetails show
ShowUninstDetails show
SetCompressor /SOLID lzma

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "CompanyName" "${APP_PUBLISHER}"
VIAddVersionKey "FileDescription" "${APP_NAME} 安装程序"
VIAddVersionKey "FileVersion" "${VERSION}.0"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "LegalCopyright" "${APP_PUBLISHER}"

!define MUI_ABORTWARNING
!define MUI_ICON "..\..\desktop\resources\icon.ico"
!define MUI_UNICON "..\..\desktop\resources\icon.ico"
!define MUI_FINISHPAGE_RUN "$INSTDIR\${APP_EXE}"
!define MUI_FINISHPAGE_RUN_TEXT "立即启动 ${APP_NAME}"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "SimpChinese"

Section "安装 ${APP_NAME}" SecMain
  ; 32 位系统上不提供安装：程序本身是 x64 构建
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "本程序为 64 位版本，请在 64 位 Windows 上安装。"
    Abort
  ${EndIf}

  SetOutPath "$INSTDIR"
  File "${STAGE}\${APP_EXE}"
  File /nonfatal "${STAGE}\README.md"
  File /nonfatal "${STAGE}\event_schema.json"

  SetOutPath "$INSTDIR\resources"
  File /nonfatal /r "${STAGE}\resources\*"

  SetOutPath "$INSTDIR\adapters"
  File /nonfatal /r "${STAGE}\adapters\*"

  ; 安装目录记进注册表：重装/卸载时能找回位置
  WriteRegStr HKCU "${APP_REGKEY}" "InstallDir" "$INSTDIR"

  CreateDirectory "$SMPROGRAMS\${APP_NAME}"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"
  CreateShortcut "$SMPROGRAMS\${APP_NAME}\卸载.lnk" "$INSTDIR\unins000.exe"
  CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}"

  WriteRegStr HKCU "${APP_UNINSTKEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "${APP_UNINSTKEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${APP_UNINSTKEY}" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr HKCU "${APP_UNINSTKEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${APP_UNINSTKEY}" "DisplayIcon" "$INSTDIR\${APP_EXE}"
  WriteRegStr HKCU "${APP_UNINSTKEY}" "UninstallString" '"$INSTDIR\unins000.exe"'
  WriteRegStr HKCU "${APP_UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\unins000.exe" /S'
  WriteRegDWORD HKCU "${APP_UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${APP_UNINSTKEY}" "NoRepair" 1
  WriteRegDWORD HKCU "${APP_UNINSTKEY}" "EstimatedSize" 0

  WriteUninstaller "$INSTDIR\unins000.exe"
SectionEnd

Section "Uninstall"
  ; 先删自启动项，避免卸载后仍尝试启动已删除的程序
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "AI Task Hub Win32"

  Delete "$INSTDIR\${APP_EXE}"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\event_schema.json"
  ; Delete 不支持 /r：整棵目录树用 RMDir /r
  RMDir /r "$INSTDIR\resources"
  RMDir /r "$INSTDIR\adapters"
  Delete "$DESKTOP\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\${APP_NAME}.lnk"
  Delete "$SMPROGRAMS\${APP_NAME}\卸载.lnk"
  RMDir "$SMPROGRAMS\${APP_NAME}"
  ; 只在目录为空时删除：data.sqlite / AI Task Hub.ini 属于用户个人数据，保留
  RMDir "$INSTDIR"
  ; 卸载程序自身正在运行，无法当场删除：安排在下次重启时清理
  Delete /REBOOTOK "$INSTDIR\unins000.exe"
  RMDir /REBOOTOK "$INSTDIR"
  DeleteRegKey HKCU "${APP_UNINSTKEY}"
  DeleteRegKey HKCU "${APP_REGKEY}"
SectionEnd
