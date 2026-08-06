import { spawnSync } from 'node:child_process'
import fs from 'node:fs'
import os from 'node:os'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

// ---------------------------------------------------------------------------
// electron-builder 统一入口
//
// 背景：electron-builder 复用 desktop/dist/win-unpacked/，会先删除旧的
// app.asar，而该文件可能被无关进程（如 Typora）持久占用，导致
// EBUSY: unlink '...\desktop\dist\win-unpacked\resources\app.asar'。
//
// 方案：把 electron-builder 输出目录指到项目树外每次唯一的临时目录
// （staging），构建完成后把顶层最终产物拷回 desktop/dist，从而永不触碰
// 被锁住的旧产物。
// ---------------------------------------------------------------------------

const scriptDir = path.dirname(fileURLToPath(import.meta.url))
const desktopRoot = path.resolve(scriptDir, '..')
const distDir = path.join(desktopRoot, 'dist')

// 唯一临时输出目录（不预清理；进程退出后由 finally 删除）
const staging = path.join(os.tmpdir(), `ai-task-hub-build-${process.pid}-${Date.now()}`)

// staging 顶层需要拷回 desktop/dist 的最终产物
const ALWAYS_COPY = ['latest.yml']
const OPTIONAL_COPY = ['builder-debug.yml']
const ARTIFACT_RE =
  /^AI Task Hub Setup .+\.exe(\.blockmap)?$|^AI-Task-Hub-Portable-.+\.exe$/

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms))

// ---------------------------------------------------------------------------
// 重试：覆盖目标被占用（EBUSY/EPERM）时重试 5 次 × 200ms 退避
// ---------------------------------------------------------------------------

async function withRetry(label, fn) {
  const MAX_ATTEMPTS = 5
  const BACKOFF_MS = 200
  let lastErr
  for (let attempt = 1; attempt <= MAX_ATTEMPTS; attempt++) {
    try {
      return fn()
    } catch (err) {
      lastErr = err
      if (err.code !== 'EBUSY' && err.code !== 'EPERM') throw err
      if (attempt < MAX_ATTEMPTS) {
        console.error(`[electron-dist] ${label} 被占用（${err.code}），第 ${attempt}/${MAX_ATTEMPTS} 次重试…`)
        await sleep(BACKOFF_MS)
      }
    }
  }
  throw lastErr
}

class LockedFileError extends Error {
  constructor(filePath) {
    super(`无法覆盖产物：${filePath}`)
    this.name = 'LockedFileError'
    this.filePath = filePath
  }
}

async function copyWithRetry(src, dest) {
  try {
    await withRetry(path.basename(dest), () => fs.copyFileSync(src, dest))
  } catch (err) {
    if (err.code === 'EBUSY' || err.code === 'EPERM') {
      throw new LockedFileError(dest)
    }
    throw err
  }
}

// ---------------------------------------------------------------------------
// latest.yml 原子写：先写 .tmp 再 rename 覆盖，随后读回校验 version 一致
// ---------------------------------------------------------------------------

function readYamlVersion(filePath) {
  const text = fs.readFileSync(filePath, 'utf-8')
  const m = text.match(/^version:\s*["']?([^"'\s]+)["']?\s*$/m)
  return m ? m[1] : null
}

async function atomicWriteYaml(src, dest) {
  const tmp = path.join(distDir, 'latest.yml.tmp')
  try {
    // 先写临时文件，再原子 rename 覆盖，避免读到写一半的内容
    await copyWithRetry(src, tmp)
    await withRetry('latest.yml', () => fs.renameSync(tmp, dest))
    // 拷后读回解析 version，与 electron-builder 生成的一致才视为成功
    const from = readYamlVersion(src)
    const written = readYamlVersion(dest)
    if (from !== written) {
      throw new Error(`latest.yml version 校验不一致：staging=${from}，dist=${written}`)
    }
  } catch (err) {
    try { fs.rmSync(tmp, { force: true }) } catch { /* best-effort */ }
    throw err
  }
}

// ---------------------------------------------------------------------------
// 拷贝最终产物
// ---------------------------------------------------------------------------

async function copyArtifacts() {
  fs.mkdirSync(distDir, { recursive: true })

  let names = []
  try {
    names = fs.readdirSync(staging)
  } catch {
    names = []
  }

  const toCopy = names.filter(
    (name) => ALWAYS_COPY.includes(name) || OPTIONAL_COPY.includes(name) || ARTIFACT_RE.test(name),
  )
  if (toCopy.length === 0) {
    console.warn('[electron-dist] staging 顶层未找到匹配产物，请检查 electron-builder 是否成功生成。')
  }

  for (const name of toCopy) {
    const src = path.join(staging, name)
    const dest = path.join(distDir, name)
    if (name === 'latest.yml') {
      await atomicWriteYaml(src, dest)
    } else {
      await copyWithRetry(src, dest)
    }
  }

  console.log('[electron-dist] 已拷回 desktop/dist：')
  for (const name of toCopy) {
    console.log(`  - ${name}`)
  }
}

// ---------------------------------------------------------------------------
// 用 Restart Manager（rstrtmgr.dll 的 RmGetList）枚举占用文件路径的进程
// ---------------------------------------------------------------------------

const RM_LOCKER_PS1 = `
param([string]$TargetPath)

Add-Type -TypeDefinition @"
using System;
using System.Runtime.InteropServices;

public static class AiTaskHubRm {
    [StructLayout(LayoutKind.Sequential)]
    public struct FILETIME {
        public uint dwLowDateTime;
        public uint dwHighDateTime;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct RM_UNIQUE_PROCESS {
        public uint dwProcessId;
        public FILETIME ProcessStartTime;
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    public struct RM_PROCESS_INFO {
        public RM_UNIQUE_PROCESS Process;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 256)]
        public string strAppName;
        [MarshalAs(UnmanagedType.ByValTStr, SizeConst = 64)]
        public string strServiceShortName;
        public uint ApplicationType;
        public uint AppStatus;
        public uint TSSessionId;
        [MarshalAs(UnmanagedType.Bool)]
        public bool bRestartable;
    }

    [DllImport("rstrtmgr.dll", CharSet = CharSet.Unicode)]
    public static extern uint RmStartSession(out uint pSessionHandle, uint dwSessionFlags, string strSessionKey);

    [DllImport("rstrtmgr.dll", CharSet = CharSet.Unicode)]
    public static extern uint RmRegisterResources(uint dwSessionHandle, uint nFiles, string[] rgsFilenames, uint nApplications, IntPtr rgApplications, uint nServices, string[] rgsServiceNames);

    [DllImport("rstrtmgr.dll")]
    public static extern uint RmGetList(uint dwSessionHandle, out uint pnProcInfoNeeded, ref uint pnProcInfo, [In, Out] RM_PROCESS_INFO[] rgAffectedApps, ref uint lpdwRebootReasons);

    [DllImport("rstrtmgr.dll")]
    public static extern uint RmEndSession(uint dwSessionHandle);
}
"@

function Get-RmLockers([string]$Path) {
    $sessionKey = [Guid]::NewGuid().ToString()
    $sessionHandle = [UInt32]0
    $hr = [AiTaskHubRm]::RmStartSession([ref]$sessionHandle, 0, $sessionKey)
    if ($hr -ne 0) { return }
    try {
        $hr = [AiTaskHubRm]::RmRegisterResources($sessionHandle, 1, @($Path), 0, [IntPtr]::Zero, 0, @())
        if ($hr -ne 0) { return }
        $needed = [UInt32]0
        $count = [UInt32]0
        $reasons = [UInt32]0
        [void][AiTaskHubRm]::RmGetList($sessionHandle, [ref]$needed, [ref]$count, $null, [ref]$reasons)
        if ($needed -eq 0) { return }
        $count = $needed
        $infos = [AiTaskHubRm+RM_PROCESS_INFO[]]::new($needed)
        $hr = [AiTaskHubRm]::RmGetList($sessionHandle, [ref]$needed, [ref]$count, $infos, [ref]$reasons)
        if ($hr -ne 0) { return }
        for ($i = 0; $i -lt $count; $i++) {
            $p = $infos[$i]
            if ($p.Process.dwProcessId -gt 0) {
                "{0}{1}{2}" -f $p.Process.dwProcessId, [char]9, $p.strAppName
            }
        }
    } finally {
        [void][AiTaskHubRm]::RmEndSession($sessionHandle)
    }
}

if (-not [string]::IsNullOrWhiteSpace($TargetPath)) {
    Get-RmLockers $TargetPath
}
`

function findLockingProcesses(filePath) {
  const ps1 = path.join(os.tmpdir(), `ai-task-hub-rm-lockers-${process.pid}.ps1`)
  try {
    fs.writeFileSync(ps1, RM_LOCKER_PS1, 'utf-8')
    const result = spawnSync(
      'powershell',
      ['-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', ps1, filePath],
      { encoding: 'utf-8', windowsHide: true, timeout: 30000 },
    )
    const lockers = []
    if (result.status === 0 && result.stdout) {
      for (const line of result.stdout.split(/\r?\n/)) {
        const m = line.match(/^(\d+)\t(.+)$/)
        if (m) lockers.push({ pid: Number(m[1]), name: m[2].trim() })
      }
    }
    return lockers
  } catch {
    return []
  } finally {
    try { fs.rmSync(ps1, { force: true }) } catch { /* best-effort */ }
  }
}

function reportLockedFile(filePath) {
  console.error('')
  console.error(`[electron-dist] 无法覆盖产物：${filePath}`)
  const lockers = findLockingProcesses(filePath)
  if (lockers.length > 0) {
    console.error('该文件被以下进程占用：')
    for (const { pid, name } of lockers) {
      console.error(`  - ${name}（PID ${pid}）`)
    }
  } else {
    console.error('未能枚举占用进程，可能被系统进程或其它句柄持有。')
  }
  console.error('请退出占用进程后重新执行打包。')
}

// ---------------------------------------------------------------------------
// 主流程
// ---------------------------------------------------------------------------

async function main() {
  try {
    // 输出目录指到唯一的 staging，electron-builder 不再触碰旧的 dist
    console.log(`[electron-dist] 输出目录：${staging}`)

    const result = spawnSync(
      'node',
      ['node_modules/electron-builder/cli.js', '--config.directories.output=' + staging, ...process.argv.slice(2)],
      { cwd: desktopRoot, stdio: 'inherit', env: process.env },
    )

    if (result.error) {
      console.error('[electron-dist] 启动 electron-builder 失败：', result.error.message)
      process.exitCode = 1
      return
    }
    if (result.status !== 0) {
      process.exitCode = result.status ?? 1
      return
    }

    await copyArtifacts()

    console.log('')
    console.log('[electron-dist] 打包完成，产物位于 desktop/dist。')
    console.log('[electron-dist] 提示：desktop/dist/win-unpacked 为中间产物，本地打包不再刷新；')
    console.log('[electron-dist]       如被占用进程（如 Typora）钉住，可在退出占用后手动删除该目录。')
  } catch (err) {
    if (err instanceof LockedFileError) {
      reportLockedFile(err.filePath)
    } else {
      console.error('[electron-dist] 打包失败：')
      console.error(err)
    }
    process.exitCode = 1
  } finally {
    try {
      fs.rmSync(staging, { recursive: true, force: true })
    } catch { /* best-effort */ }
  }
}

main()
