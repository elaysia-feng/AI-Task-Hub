# PyInstaller 打包配置：aihub-backend.exe（FastAPI + uvicorn + MySQL）
# 构建：pyinstaller packaging/backend.spec --distpath packaging/dist --workpath packaging/build --noconfirm

import os

from PyInstaller.utils.hooks import collect_submodules

ROOT = os.path.abspath(os.path.join(SPECPATH, ".."))

# uvicorn 按配置动态导入 lifespan/loops/protocols，静态分析抓不到，全量收集；
# app 与 shared 同理（工厂函数在运行时才装配路由）
hiddenimports = (
    collect_submodules("uvicorn")
    + collect_submodules("app")
    + collect_submodules("shared")
    + collect_submodules("websockets")
)

a = Analysis(
    [os.path.join(ROOT, "app", "main.py")],
    pathex=[ROOT],
    binaries=[],
    datas=[
        (os.path.join(ROOT, "app", "database", "schema.sql"), "app/database"),
        (os.path.join(ROOT, "app", "database", "schema_sqlite.sql"), "app/database"),
        (os.path.join(ROOT, "shared", "event_schema.json"), "shared"),
        # ChatGPT 扩展：打包态由 _chatgpt_extension_dir() 物化到 %APPDATA%/AI Task Hub/chatgpt-extension
        # （_MEIPASS 是每次运行临时解压目录，Chrome 卸载扩展须指向稳定路径）
        (os.path.join(ROOT, "adapters", "chatgpt-extension"), "adapters/chatgpt-extension"),
        # Claude Code / Codex 适配器（纯标准库，由本机系统 Python 执行）：打包态物化到
        # %APPDATA%/AI Task Hub/adapters/*；forward_target.json / notify_debug.log 为运行时产物，
        # 物化时排除（见 _codex_chain() 的 exclude 集合）
        (os.path.join(ROOT, "adapters", "claude-code"), "adapters/claude-code"),
        (os.path.join(ROOT, "adapters", "codex"), "adapters/codex"),
    ],
    hiddenimports=hiddenimports,
    hookspath=[],
    hooksconfig={},
    runtime_hooks=[],
    excludes=[],
    noarchive=False,
)

pyz = PYZ(a.pure)

exe = EXE(
    pyz,
    a.scripts,
    a.binaries,
    a.datas,
    [],
    name="aihub-backend",
    debug=False,
    bootloader_ignore_signals=False,
    strip=False,
    upx=False,
    upx_exclude=[],
    runtime_tmpdir=None,
    console=False,
    disable_windowed_traceback=False,
    argv_emulation=False,
    target_arch=None,
    codesign_identity=None,
    entitlements_file=None,
    icon=os.path.join(ROOT, "packaging", "app.ico"),
)
