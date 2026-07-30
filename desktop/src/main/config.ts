import path from 'node:path'
import { app } from 'electron'

/** 仓库根目录（desktop/out/main → 上三级），开发模式下用于拉起 Python 后端 */
export const REPO_ROOT = path.resolve(__dirname, '..', '..', '..')

export const API_PORT = 17891
export const API_BASE = `http://127.0.0.1:${API_PORT}`
export const WS_URL = `ws://127.0.0.1:${API_PORT}/ws/tasks`
export const HEALTH_URL = `${API_BASE}/api/health`

/**
 * 后端启动命令：
 * 打包态 → resources 内嵌的 aihub-backend.exe（PyInstaller 单文件）
 * 开发态 → 仓库 .venv 的 python -m app.main
 */
export const BACKEND_CMD: { exe: string; args: string[] } = app.isPackaged
  ? { exe: path.join(process.resourcesPath, 'backend', 'aihub-backend.exe'), args: [] }
  : {
      exe: process.env.AIHUB_PYTHON ?? path.join(REPO_ROOT, '.venv', 'Scripts', 'python.exe'),
      args: ['-m', 'app.main'],
    }

export const BACKEND_CWD = app.isPackaged ? path.dirname(BACKEND_CMD.exe) : REPO_ROOT

export const RESOURCES_DIR = path.resolve(__dirname, '..', '..', 'resources')
