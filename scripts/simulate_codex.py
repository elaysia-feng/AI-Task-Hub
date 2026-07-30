"""忠实模拟 Codex CLI 调用 notify_chain.py（绕过 PowerShell 引号吞字问题）。

用法：.venv/Scripts/python.exe scripts/simulate_codex.py
"""

import json
import subprocess
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ADAPTER = ROOT / "adapters" / "codex" / "notify_chain.py"

payload = {
    "type": "agent-turn-complete",
    "thread-id": "11111111-2222-3333-4444-555555555555",
    "turn-id": "7",
    "cwd": str(ROOT).replace("\\", "/"),
    "input-messages": ["排查 codex notify 不触发问题"],
    "last-assistant-message": "已修复：notify 钩子正常触发",
}

# 与 Codex 相同的调用方式：程序 + 参数数组，JSON 作为单个 argv
proc = subprocess.run(
    [sys.executable, str(ADAPTER), json.dumps(payload, ensure_ascii=False)],
    capture_output=True,
    text=True,
    cwd=str(ROOT),
)
print("adapter exit:", proc.returncode)
if proc.stdout:
    print("stdout:", proc.stdout[:500])
if proc.stderr:
    print("stderr:", proc.stderr[:500])

opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
with opener.open("http://127.0.0.1:17891/api/tasks?view=queue", timeout=3) as res:
    queue = json.loads(res.read())["tasks"]
print("queue count:", len(queue))
for t in queue:
    print(f"  [{t['source']}] {t['title']}  status={t['status']}  ext={t['externalTaskId']}")
