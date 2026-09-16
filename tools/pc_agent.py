"""局域网关机代理：供 ESP8266 唤醒器调用，执行 Windows 远程关机。

用法: python pc_agent.py <token> [端口，默认 8899]
  - token 与固件 config.h 里的 AGENT_TOKEN 一致
  - 环境变量 AGENT_DRYRUN=1 时仅应答不真正关机（链路测试用）
  - /health 端点无需 token，可用于探活

建议用任务计划程序以 SYSTEM 账户开机自启（登录前即可用）。
"""
import os
import subprocess
import sys
from http.server import HTTPServer, BaseHTTPRequestHandler

TOKEN = sys.argv[1] if len(sys.argv) > 1 else ""
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 8899
DRYRUN = os.environ.get("AGENT_DRYRUN") == "1"


class Handler(BaseHTTPRequestHandler):
    def _reply(self, code, body):
        self.send_response(code)
        self.send_header("content-type", "application/json")
        self.end_headers()
        self.wfile.write(body.encode())

    def do_GET(self):
        if self.path.startswith("/health"):
            self._reply(200, '{"ok":true,"dryrun":%s}' % ("true" if DRYRUN else "false"))
        elif self.path.startswith(f"/shutdown?token={TOKEN}"):
            if DRYRUN:
                self._reply(200, '{"ok":true,"dryrun":true}')
            else:
                # /t 5 给 ESP 回应留 5 秒窗口；可用 shutdown /a 中止
                subprocess.Popen(["shutdown", "/s", "/t", "5", "/c", "WoL 远程关机"])
                self._reply(200, '{"ok":true}')
        else:
            self._reply(404, '{"error":"not_found"}')

    def log_message(self, fmt, *args):
        print("[agent]", self.address_string(), fmt % args, flush=True)


if not TOKEN:
    sys.exit("缺少 token 参数")

print(f"[agent] 监听 0.0.0.0:{PORT}  dryrun={DRYRUN}", flush=True)
HTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
