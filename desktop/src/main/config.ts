import path from 'node:path'

/** 仓库根目录（desktop/out/main → 上三级），用于拉起 Python 后端 */
export const REPO_ROOT = path.resolve(__dirname, '..', '..', '..')

export const API_PORT = 17891
export const API_BASE = `http://127.0.0.1:${API_PORT}`
export const WS_URL = `ws://127.0.0.1:${API_PORT}/ws/tasks`
export const HEALTH_URL = `${API_BASE}/api/health`

export const BACKEND_PYTHON =
  process.env.AIHUB_PYTHON ?? path.join(REPO_ROOT, '.venv', 'Scripts', 'python.exe')

export const RESOURCES_DIR = path.resolve(__dirname, '..', '..', 'resources')
