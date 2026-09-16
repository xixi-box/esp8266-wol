# esp8266-wol — 远程开机工具

ESP8266 + Cloudflare Workers 组成的远程开机（Wake-on-LAN）工具：
在任意网络环境下用手机/浏览器唤醒局域网内的电脑，无需公网 IP、端口映射或内网穿透。

## 架构

```
手机/浏览器 ──HTTPS──> Cloudflare Worker（KV 命令队列 + 状态页）
                            │
                            ▼ 每 3 秒轮询 GET /api/command（纯读）
                      ESP8266（局域网内代理人）
                            │ 收到 wake 命令
                            ▼ UDP 广播 WoL 魔术包
                        目标电脑开机
```

设计要点：命令带自增 id 且设备回报 acked，保证幂等（断电重启不会重复开机）；
轮询纯读、心跳 5 分钟一次，避开 KV 免费额度（写 1000 次/天）。

## 目录

- `firmware/` — ESP8266 固件（PlatformIO / Arduino 框架）
- `worker/`   — Cloudflare Worker（KV 命令队列 + 网页按钮）

## 上手步骤

### 1. 配置并烧录固件

编辑 `firmware/include/config.h`（从 config.h.example 复制）：填 WiFi 名称/密码（2.4GHz）、Worker 地址、
DEVICE_TOKEN（与 Worker 一致）、目标电脑有线网卡 MAC。

```bash
cd firmware
pio run -t upload          # 编译并烧录（COM 口自动识别）
pio device monitor         # 看串口日志（115200）
```

### 2. 部署 Worker

```bash
cd worker
npm install
npx wrangler login                          # 浏览器授权（只需一次）
npx wrangler kv namespace create CMD        # 把输出的 id 填入 wrangler.toml
npx wrangler secret put DEVICE_TOKEN        # 粘贴与固件相同的 token
npx wrangler secret put USER_TOKEN          # 自定一串随机字符，用于网页/接口鉴权
npx wrangler deploy
```

自定义域名在 `wrangler.toml` 的 `routes` 中配置（`wol.wangshun.work`），
要求该域名已接入 Cloudflare。不要用 `*.workers.dev` —— 它在国内被墙
（DNS 污染 + SNI 阻断），ESP8266 和手机流量网络都连不上。

### 3. 使用

浏览器打开 `https://wol.wangshun.work/?token=<USER_TOKEN>`：
- **开机按钮**：点击后指令经 Cloudflare 中转，ESP8266 在 3 秒内收到并广播魔术包
- **设置**：可远程修改唤醒器的 WiFi 名称/密码和目标网卡 MAC，设备在 5 分钟内心跳时自动应用
- 也可以把该链接存成手机书签/快捷方式

### 前提条件（一次性）

目标电脑需要开启 WoL：BIOS 里启用 Wake on LAN / PCIe 设备唤醒、关闭 ErP；
网卡驱动属性中"魔术封包唤醒"已勾选。**关机状态下可靠唤醒必须走有线网卡**
（无线网卡通常只支持从睡眠唤醒）。验证方法：电脑关机后用手机 WoL App
在同一局域网内试发魔术包。

### 应急恢复

如果远程改坏了 WiFi 配置，设备连不上网时会自动开启名为 **WoL-Setup** 的
开放热点；手机连上后访问 `http://192.168.4.1` 即可重新配置（板载 LED 慢闪
表示处于热点模式）。配置保存在设备 EEPROM 中，Worker 设置页的远程配置
（rev 版本号新于本机时）会覆盖它。
