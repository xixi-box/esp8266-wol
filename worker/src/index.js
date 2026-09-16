/**
 * ESP8266 远程开机 - Cloudflare Worker（命令中转 + 状态/设置页）
 *
 * 鉴权：
 *   设备接口 → Authorization: Bearer <DEVICE_TOKEN>
 *   用户接口 → ?token=<USER_TOKEN>
 *
 * KV 免费额度：读 10 万/天、写 1000/天。
 * 因此设备轮询 /api/command 是纯读；只有命令写入、低频心跳和设置修改产生写操作。
 *
 * 远程配置：设置页 → POST /api/config → KV("config")，带 rev 版本号；
 * 设备心跳时取回配置，rev 更新则应用（WiFi/MAC 均可远程改）。
 *
 * 部署前准备：
 *   1. npx wrangler kv namespace create CMD   → 把返回的 id 填进 wrangler.toml
 *   2. npx wrangler secret put DEVICE_TOKEN
 *   3. npx wrangler secret put USER_TOKEN
 */

const json = (data, status = 200) =>
  new Response(JSON.stringify(data), {
    status,
    headers: { "content-type": "application/json" },
  });

const esc = (s) =>
  String(s ?? "").replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/"/g, "&quot;");

const normalizeMac = (s) => String(s ?? "").replace(/[^0-9a-fA-F]/g, "").toUpperCase();

function isDevice(request, env) {
  return request.headers.get("Authorization") === `Bearer ${env.DEVICE_TOKEN}`;
}

function isUser(url, env) {
  return url.searchParams.get("token") === env.USER_TOKEN;
}

function statusPage(online, pcOnline, pcTs, cfg) {
  return `<!doctype html>
<html lang="zh"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<meta name="robots" content="noindex">
<title>远程开机</title>
<style>
 body{font-family:system-ui,sans-serif;max-width:460px;margin:6vh auto;padding:0 16px;color:#1f2937}
 button{font-size:18px;padding:14px 40px;border-radius:12px;border:0;background:#2563eb;color:#fff;width:100%;cursor:pointer}
 button:disabled{background:#9ca3af;cursor:default}
 .st{margin:16px 0;color:#6b7280}
 .ok{color:#16a34a;font-weight:600}.bad{color:#dc2626;font-weight:600}
 #msg,#cfgmsg{min-height:22px;margin:10px 0;color:#6b7280}
 details{margin-top:28px;border-top:1px solid #e5e7eb;padding-top:14px}
 summary{cursor:pointer;font-weight:600;color:#374151}
 label{display:block;font-size:14px;color:#6b7280;margin-top:10px}
 input{width:100%;padding:10px;margin-top:4px;box-sizing:border-box;border:1px solid #d1d5db;border-radius:8px;font-size:16px}
 .hint{font-size:12px;color:#9ca3af;margin-top:8px}
</style></head><body>
<h2>🖥️ 远程开机</h2>
<div class="st">唤醒器状态：<span id="sttext" class="${online ? "ok" : "bad"}">${online ? "在线" : "离线"}</span><span id="stextra" class="st"></span></div>
<div class="st">电脑状态：<span id="pctext" class="${pcOnline ? "ok" : "bad"}">${pcOnline ? "在线" : "离线"}</span><span id="pcextra" class="st">${pcTs ? "" : "（暂无数据）"}</span></div>
<button id="b" onclick="wake()">开　机</button>
<div id="msg"></div>
<details><summary>⚙️ 设置（WiFi / 目标 MAC）</summary>
<form onsubmit="return saveCfg(event)">
<label>WiFi 名称（2.4GHz）<input name="ssid" value="${cfg ? esc(cfg.ssid) : ""}" maxlength="32"></label>
<label>WiFi 密码<input name="pass" type="password" placeholder="留空保持不变" maxlength="64"></label>
<label>目标网卡 MAC（12 位十六进制，可带 : - 分隔符）<input name="mac" value="${cfg ? esc(cfg.mac) : ""}" placeholder="如 B025AA89BEF5"></label>
<label>电脑局域网 IP（用于开机状态监测）<input name="pcip" value="${cfg && cfg.pc_ip ? esc(cfg.pc_ip) : ""}" placeholder="留空保持不变"></label>
<button type="submit">保存设置</button>
</form>
<div id="cfgmsg"></div>
<div class="hint">保存后设备会在 5 分钟内心跳时自动应用；想立即生效就给唤醒器断电重启。若改坏了 WiFi，设备会自动开启名为 WoL-Setup 的热点，手机连上后访问 192.168.4.1 修复。</div>
</details>
<script>
var token = new URLSearchParams(location.search).get('token');
function refreshStatus(){
  fetch('/api/status?token=' + encodeURIComponent(token))
    .then(function(r){ return r.json(); })
    .then(function(j){
      var el = document.getElementById('sttext');
      var ex = document.getElementById('stextra');
      el.textContent = j.online ? '在线' : '离线';
      el.className = j.online ? 'ok' : 'bad';
      ex.textContent = j.online ? '' :
        (j.ts ? '（最后心跳 ' + new Date(j.ts).toLocaleTimeString('zh-CN') + '）' : '（从未上报）');
      var pe = document.getElementById('pctext');
      var pex = document.getElementById('pcextra');
      pe.textContent = j.pcOnline ? '在线' : '离线';
      pe.className = j.pcOnline ? 'ok' : 'bad';
      pex.textContent = j.pcOnline ? '' :
        (j.pcTs ? '（最后在线 ' + new Date(j.pcTs).toLocaleTimeString('zh-CN') + '）' : '（暂无数据）');
    })
    .catch(function(){ /* 保持当前显示 */ });
}
setInterval(refreshStatus, 30000);
function wake(){
  var b = document.getElementById('b');
  b.disabled = true; b.textContent = '发送中…';
  fetch('/api/wake?token=' + encodeURIComponent(token), { method: 'POST' })
    .then(function(r){
      var m = document.getElementById('msg');
      if (r.ok) { m.textContent = '指令已发送，电脑应在数秒内开机'; m.style.color = '#16a34a';
                  b.textContent = '已发送 ✓';
                  setTimeout(function(){ b.disabled = false; b.textContent = '开　机'; }, 10000); }
      else { m.textContent = '发送失败（HTTP ' + r.status + '）'; m.style.color = '#dc2626';
             b.disabled = false; b.textContent = '重　试'; }
    })
    .catch(function(){ var m=document.getElementById('msg');
      m.textContent = '网络错误，请重试'; m.style.color = '#dc2626';
      b.disabled = false; b.textContent = '重　试'; });
}
function saveCfg(ev){
  ev.preventDefault();
  var f = ev.target;
  fetch('/api/config?token=' + encodeURIComponent(token), {
      method: 'POST',
      headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ ssid: f.ssid.value.trim(), pass: f.pass.value, mac: f.mac.value.trim(), pc_ip: f.pcip.value.trim() })
    })
    .then(function(r){ return r.json().then(function(j){ return { s: r.status, j: j }; }); })
    .then(function(x){
      var m = document.getElementById('cfgmsg');
      if (x.s === 200) { m.textContent = '已保存（rev ' + x.j.rev + '），设备将在 5 分钟内自动应用';
                         m.style.color = '#16a34a'; f.pass.value = ''; }
      else { m.textContent = '保存失败：' + (x.j.error || ('HTTP ' + x.s)); m.style.color = '#dc2626'; }
    })
    .catch(function(){ var m = document.getElementById('cfgmsg');
      m.textContent = '网络错误，请重试'; m.style.color = '#dc2626'; });
  return false;
}
</script></body></html>`;
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const { pathname } = url;

    // ---- 设备轮询：纯读。acked 为设备已执行的最后命令 id，相同则不下发（幂等） ----
    if (pathname === "/api/command" && request.method === "GET") {
      if (!isDevice(request, env)) return json({ error: "unauthorized" }, 401);
      const acked = Number(url.searchParams.get("acked") || "0");
      const cmd = await env.CMD.get("cmd", "json");
      if (!cmd || cmd.id === acked) return json({ command: null, id: acked });
      return json(cmd);
    }

    // ---- 设备心跳：每 5 分钟一次；响应顺带带回最新配置（省一次请求） ----
    if (pathname === "/api/heartbeat" && request.method === "POST") {
      if (!isDevice(request, env)) return json({ error: "unauthorized" }, 401);
      let extra = {};
      try { extra = await request.json(); } catch { /* body 可为空 */ }
      await env.CMD.put("heartbeat", JSON.stringify({ ts: Date.now(), ...extra }));
      const cfg = await env.CMD.get("config", "json");
      return json(cfg ? { ok: true, config: cfg } : { ok: true });
    }

    // ---- PC 开关机状态（设备边沿触发上报，仅变化时写 KV） ----
    if (pathname === "/api/pcstate" && request.method === "POST") {
      if (!isDevice(request, env)) return json({ error: "unauthorized" }, 401);
      const body = await request.json().catch(() => null);
      await env.CMD.put("pcstate", JSON.stringify({ ts: Date.now(), online: !!body?.online }));
      return json({ ok: true });
    }

    // ---- 触发开机：写入命令 ----
    if (pathname === "/api/wake" && request.method === "POST") {
      if (!isUser(url, env)) return json({ error: "unauthorized" }, 401);
      const id = Date.now();
      await env.CMD.put("cmd", JSON.stringify({ command: "wake", id }));
      return json({ ok: true, id });
    }

    // ---- 修改远程配置（WiFi / MAC） ----
    if (pathname === "/api/config" && request.method === "POST") {
      if (!isUser(url, env)) return json({ error: "unauthorized" }, 401);
      const body = await request.json().catch(() => null);
      const ssid = String(body?.ssid ?? "").trim();
      const pass = String(body?.pass ?? "");
      const mac = normalizeMac(body?.mac);
      const pcIp = String(body?.pc_ip ?? "").trim();
      if (!ssid || ssid.length > 32) return json({ error: "WiFi 名称无效（1-32 字符）" }, 400);
      if (pass.length > 64) return json({ error: "密码过长（≤64 字符）" }, 400);
      if (mac.length !== 12) return json({ error: "MAC 无效（需要 12 位十六进制）" }, 400);
      if (pcIp && !/^\d{1,3}(\.\d{1,3}){3}$/.test(pcIp)) return json({ error: "电脑 IP 格式无效" }, 400);
      const cur = await env.CMD.get("config", "json");
      const finalPass = pass || cur?.pass || "";
      const finalPcIp = pcIp || cur?.pc_ip || "";
      const rev = Date.now();
      await env.CMD.put("config", JSON.stringify({ rev, ssid, pass: finalPass, mac, pc_ip: finalPcIp }));
      return json({ ok: true, rev });
    }

    // ---- 状态查询（页面每 30 秒自动刷新用） ----
    if (pathname === "/api/status" && request.method === "GET") {
      if (!isUser(url, env)) return json({ error: "unauthorized" }, 401);
      const hb = await env.CMD.get("heartbeat", "json");
      const pc = await env.CMD.get("pcstate", "json");
      const online = !!hb && Date.now() - hb.ts < 6 * 60_000;
      const pcOnline = !!pc && Date.now() - pc.ts < 10 * 60_000;
      return json({ online, ts: hb?.ts ?? 0, pcOnline, pcTs: pc?.ts ?? 0 });
    }

    // ---- 状态 + 一键开机 + 设置页面 ----
    if (pathname === "/" && request.method === "GET") {
      if (!isUser(url, env)) return new Response("unauthorized", { status: 401 });
      const hb = await env.CMD.get("heartbeat", "json");
      const cfg = await env.CMD.get("config", "json");
      const pc = await env.CMD.get("pcstate", "json");
      const online = !!hb && Date.now() - hb.ts < 6 * 60_000;
      const pcOnline = !!pc && Date.now() - pc.ts < 10 * 60_000;
      return new Response(statusPage(online, pcOnline, pc?.ts ?? 0, cfg), {
        headers: { "content-type": "text/html; charset=utf-8", "cache-control": "no-store" },
      });
    }

    return json({ error: "not_found" }, 404);
  },
};
