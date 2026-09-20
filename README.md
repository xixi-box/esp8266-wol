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
轮询纯读、心跳 3 分钟一次 + 电脑状态变化才上报，避开 KV 免费额度（写 1000 次/天）。

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
- **电脑状态**：设备每 30 秒 ping 一次电脑，页面实时显示电脑开关机——配合 UU 远程等
  远程桌面工具：点开机 → 看到"电脑状态：在线" → 直接连接
- **设置**：可远程修改唤醒器的 WiFi 名称/密码、目标网卡 MAC 和电脑 IP，设备在 3 分钟内心跳时自动应用
- 也可以把该链接存成手机书签/快捷方式

### 前提条件（一次性）

目标电脑需要开启 WoL：BIOS 里启用 Wake on LAN / PCIe 设备唤醒、关闭 ErP；
网卡驱动属性中"魔术封包唤醒"已勾选。**关机状态下可靠唤醒必须走有线网卡**
（无线网卡通常只支持从睡眠唤醒）。验证方法：电脑关机后用手机 WoL App
在同一局域网内试发魔术包。

使用「电脑状态监测」还需两个一次性设置：电脑防火墙放行 ICMP 回显（管理员运行
`netsh advfirewall firewall add rule name="WoL-ICMP" dir=in action=allow protocol=icmpv4 profile=any`），
并在路由器为电脑绑定静态 IP（之后 IP 不再变化，设置页可随时修改）。

### 应急恢复

如果远程改坏了 WiFi 配置，设备连不上网时会自动开启名为 **WoL-Setup** 的
开放热点；手机连上后访问 `http://192.168.4.1` 即可重新配置（板载 LED 慢闪
表示处于热点模式）。配置保存在设备 EEPROM 中，Worker 设置页的远程配置
（rev 版本号新于本机时）会覆盖它。

### 固件远程升级（OTA）

无需 USB 线即可升级固件。三步：

```bash
pio run                                                    # 本地构建新固件
npx wrangler kv key put firmware.bin --path .pio/build/esp12f/firmware.bin --binding=CMD --remote
npx wrangler kv key put firmware.md5 <固件md5> --binding=CMD --remote
```

再写入版本戳（构建产物内嵌的编译时间，如 `Sep 16 2026 21:13:41`，
可从串口启动日志或心跳的 fw 字段读到当前值）：

```bash
npx wrangler kv key put firmware.ver "Sep 16 2026 21:13:41" --binding=CMD --remote
```

设备在下次心跳（≤3 分钟）发现版本不同会自动下载、校验 MD5 并重启升级。
注意：下载 400+KB 固件对 WiFi 信号要求较高，弱信号下可能跨多个心跳周期重试；
升级失败设备会继续运行旧固件，不会变砖。

### 接入 Home Assistant（云端统一路径）

适合 HA 不在目标电脑局域网的情况（如部署在云服务器上）。HA 的
`configuration.yaml` 加：

```yaml
rest_command:
  wol_wake:
    url: "https://wol.wangshun.work/api/wake?token=<USER_TOKEN>"
    method: post
```

开发者工具 → YAML → 重载 REST 命令（或重启 HA）后，即可在自动化/脚本中调用。
配套脚本（`scripts:` 下）与仪表盘按钮卡片：

```yaml
script:
  wake_desktop:
    alias: 远程开机
    sequence:
      - service: rest_command.wol_wake
```

```yaml
type: button
name: 开机
icon: mdi:power
tap_action:
  action: call-service
service: script.turn_on
target:
  entity_id: script.wake_desktop
```

若 HA 就在目标电脑的局域网内，更推荐用 HA 原生 `wake_on_lan` 集成直发魔术包
（毫秒级、零云端依赖、无需 token），本云端路径留给"HA 也不在家"的场景。
可选：用 HA `rest` 平台每分钟读取 `/api/status`，把电脑在线状态也搬进仪表盘。


### 远程关机（电脑端代理）

关机无法靠魔术包（网卡硬件行为），需电脑常驻小代理（SYSTEM 账户开机自启，
登录前即生效）。管理员 PowerShell：

```powershell
netsh advfirewall firewall add rule name="WoL-Agent-8899" dir=in action=allow protocol=TCP localport=8899 profile=any
$action = New-ScheduledTaskAction -Execute "D:\dev\anaconda\pythonw.exe" -Argument '"G:\project\esp8266-wol\tools\pc_agent.py" <token> 8899'
$trigger = New-ScheduledTaskTrigger -AtStartup
$principal = New-ScheduledTaskPrincipal -UserId "SYSTEM" -LogonType ServiceAccount -RunLevel Highest
Register-ScheduledTask -TaskName "WoL-ShutdownAgent" -Action $action -Trigger $trigger -Principal $principal -Force
```

token 需与固件 config.h 的 AGENT_TOKEN 一致；代理仅监听局域网；
测试可设环境变量 `AGENT_DRYRUN=1` 只应答不真关机。
