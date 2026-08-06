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
