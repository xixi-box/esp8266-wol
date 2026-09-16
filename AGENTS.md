# esp8266-wol 项目上下文

远程开机工具：ESP8266 作为局域网代理人轮询 Cloudflare Worker 取命令，
收到 `wake` 后广播 WoL 魔术包唤醒目标电脑。详见 README.md。

## 结构

- `firmware/` — PlatformIO 项目（board=nodemcuv2，对应 ESP-12F + CH340 载板；COM 号随 USB
  插拔变化，用 `pio device list` 查）
- `worker/`   — Cloudflare Worker，KV 绑定 `CMD`，secrets：`DEVICE_TOKEN`/`USER_TOKEN`
- `tools/`    — 辅助脚本（`dns_watch.py` 直查注册局监控 NS 切换，依赖 dnspython）

## 关键约束

- **线上入口是 `https://wol.wangshun.work/?token=<USER_TOKEN>`**（自定义域，wangshun.work
  zone 在 CF，9 条 A 记录灰云直连阿里云 120.26.186.0 是站长的个人站服务，勿改勿开代理）。
  `*.workers.dev` 在国内被墙（DNS 污染+SNI 阻断），已弃用，勿再作为入口。
- KV 免费额度写 1000 次/天：`/api/command` 轮询必须纯读，心跳 3 分钟一次（480 写/天），电脑状态边沿触发上报（/api/pcstate），**不要调高频率**。
- 幂等设计：命令带 id，设备轮询带 `?acked=`，Worker 比对后决定下发；改协议时勿破坏此机制。
- `firmware/include/config.h` 含 WiFi 密码和 token，已被 .gitignore 排除，不要提交。
- ESP8266 只支持 2.4GHz WiFi；TLS 用 BearSSL + MFLN(512) + 会话恢复，证书校验为
  setInsecure（有意为之，靠 token 鉴权），若握手失败先去掉 `setBufferSizes` 排查。
- 固件配置三级覆盖：Worker KV（远程，rev 版本号） > EEPROM（本机） > config.h（出厂默认）。
  改坏 WiFi 时设备自动开热点 WoL-Setup（192.168.4.1）应急。

## 常用命令

```bash
cd firmware && pio run -t upload && pio device monitor   # 烧录 + 串口日志(115200)
cd worker && npx wrangler dev     # 本地调试（读 .dev.vars）
cd worker && npx wrangler deploy  # 部署
```

## 当前状态（2026-09-16）

- **已上线并端到端验证**：入口 `https://wol.wangshun.work/?token=<USER_TOKEN>`（token
  在 firmware/include/config.h 与 worker/.dev.vars 中，均不入库、不外泄）。
- 串口已验证：国内家宽直连 wol 域名（TLS+MFLN 200）、wake 命令→魔术包广播、心跳落 KV、
  幂等 ack（id/rev 为毫秒时间戳，设备端必须 64 位解析，勿改回 long）。
- 坑位备忘：① `*.workers.dev` 国内被墙，已用自定义域替代；② wrangler v4 的
  `kv key get/list` 默认读**本地**模拟存储，查线上数据必须加 `--remote`；
  ③ CF KV 跨区域最终一致，极速连发两条命令可能只执行最后一条（可接受）。
- 目标电脑：台式机，有线网卡 Realtek 2.5GbE（WoL 已全链路开启，**需插网线**才能关机唤醒），
  无线 MediaTek MT7922 的 WoWLAN 已启用（仅限睡眠唤醒）。设备 WiFi 信号 -83dBm 偏弱，可用。
- 电脑状态监测已上线：设备每 30 秒 ping 电脑（IP 可远程配置），边沿触发上报；
  电脑防火墙需放行 ICMP 回显（已建规则 WoL-ICMP-ESP8266），建议路由器绑定静态 IP。
- OTA 已上线：`/api/firmware` + KV 三件套（firmware.bin/ver/md5），设备心跳发现版本
  不同即自动升级重启；弱信号（-83dBm）下大文件下载可能跨多个心跳周期重试；
  升级失败自动回退继续跑旧固件。自愈兜底：连续 20 次 TLS 握手失败自动重启。
- 用户侧鉴权已加 HttpOnly Cookie（wol_token），URL token 仅首次使用后即从地址栏抹除。
