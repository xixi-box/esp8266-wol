/**
 * ESP8266 远程开机客户端 (Wake-on-LAN)
 *
 * 工作流程：
 *   1. 每 POLL_INTERVAL_MS 通过 HTTPS 轮询 Cloudflare Worker 的 /api/command
 *      （纯读请求，不消耗 KV 写额度）
 *   2. 收到 {"command":"wake","id":N} 时，向局域网广播 WoL 魔术包唤醒目标电脑
 *   3. 之后每次轮询带上 ?acked=N，Worker 比对 id 决定是否下发 —— 命令幂等，
 *      设备断电重启最多导致重发一次魔术包（对已开机的电脑无害）
 *   4. 每 HEARTBEAT_INTERVAL_MS 上报心跳，并从响应里取回远程配置：
 *      Worker 设置页修改的 WiFi/MAC 以 rev 版本号下发，rev 更新则应用并持久化
 *   5. WiFi 持续 AP_AFTER_MS 连不上时，自动开启配置热点 "WoL-Setup"，
 *      手机连上后访问 192.168.4.1 即可修改配置（防止改错 WiFi 后变砖）
 *
 * 配置三级覆盖：Worker KV（远程） > EEPROM（本机持久化） > config.h（出厂默认）
 *
 * 调试：串口监视器 115200。板载 LED：请求时短暂点亮；慢闪 = 配置热点模式。
 */

#include <Arduino.h>
#include <DNSServer.h>
#include <EEPROM.h>
#include <ESP8266HTTPClient.h>
#include <ESP8266HTTPUpdate.h>
#include <ESP8266Ping.h>
#include <ESP8266WebServer.h>
#include <ESP8266WiFi.h>
#include <WiFiClientSecure.h>
#include <WiFiUDP.h>

#include "config.h"

// 固件版本（编译时间戳），OTA 时与 Worker 上的 firmware.ver 比对
#define FW_VER __DATE__ " " __TIME__

// ---------------------------------------------------------------- 配置存取

struct DeviceConfig {
  uint32_t magic;
  char ssid[33];   // 802.11 SSID 最长 32 字节
  char pass[65];   // WPA2 密码最长 64 字节
  uint8_t mac[6];
  char pc_ip[16];  // 电脑局域网 IP，ping 它判断开关机
  int64_t rev;     // 远程配置版本号（毫秒时间戳，超出 32 位），0 = 出厂默认
  uint32_t crc;
};

static const uint32_t CFG_MAGIC = 0x574F4C31;  // "WOL1"

static DeviceConfig cfg;
static ESP8266WebServer webServer(80);
static DNSServer dnsServer;
static BearSSL::WiFiClientSecure tlsClient;
static BearSSL::Session tlsSession;  // TLS 会话恢复：握手从 ~2s 降到几百 ms
static WiFiUDP udp;

static bool apMode = false;
static uint32_t bootMs = 0;
static uint32_t lastPollMs = 0;
static uint32_t lastHeartbeatMs = 0;
static int64_t ackedId = 0;  // 已执行的最后命令 id，作为幂等游标（毫秒时间戳，64 位）

// 电脑开关机监测（边沿触发上报：仅状态变化时调 /api/pcstate，节省 KV 写额度）
static uint32_t lastPcPingMs = 0;
static bool pcOnline = false;
static int lastReportedPc = -1;  // -1 = 尚未上报过（开机首测必报）
static uint8_t pcMissCount = 0;
static uint8_t tlsFailCount = 0;  // 连续 TLS 握手失败计数，超限重启自愈

static uint32_t crc32buf(const uint8_t* data, size_t len) {
  uint32_t crc = 0xFFFFFFFF;
  while (len--) {
    crc ^= *data++;
    for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
  }
  return ~crc;
}

static void setDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  strlcpy(cfg.ssid, WIFI_SSID, sizeof(cfg.ssid));
  strlcpy(cfg.pass, WIFI_PASSWORD, sizeof(cfg.pass));
  memcpy(cfg.mac, TARGET_MAC, 6);
  strlcpy(cfg.pc_ip, DEFAULT_PC_IP, sizeof(cfg.pc_ip));
  cfg.rev = 0;
}

static void saveConfig() {
  cfg.magic = CFG_MAGIC;
  cfg.crc = crc32buf((const uint8_t*)&cfg, offsetof(DeviceConfig, crc));
  EEPROM.put(0, cfg);
  EEPROM.commit();
}

static void loadConfig() {
  EEPROM.begin(sizeof(DeviceConfig));
  DeviceConfig tmp;
  EEPROM.get(0, tmp);
  if (tmp.magic == CFG_MAGIC && tmp.rev >= 0 &&
      tmp.crc == crc32buf((const uint8_t*)&tmp, offsetof(DeviceConfig, crc))) {
    cfg = tmp;
    Serial.printf("[Cfg] 已加载本机配置 rev=%lld  SSID=%s\n", (long long)cfg.rev, cfg.ssid);
  } else {
    setDefaults();
    saveConfig();
    Serial.println("[Cfg] 使用出厂默认配置");
  }
}

// NodeMCU 板载 LED 低电平点亮
static void led(bool on) { digitalWrite(LED_BUILTIN, on ? LOW : HIGH); }

// ---------------------------------------------------------------- 解析工具

// 在 JSON 里取 "key":数字；找不到返回 -1
// 注意：id/rev 是毫秒时间戳，必须用 64 位解析，ESP8266 的 long 只有 32 位
static int64_t jsonInt(const String& json, const char* key) {
  String pat = String("\"") + key + "\"";
  int p = json.indexOf(pat);
  if (p < 0) return -1;
  p = json.indexOf(':', p + pat.length());
  if (p < 0) return -1;
  return strtoll(json.c_str() + p + 1, nullptr, 10);
}

// 在 JSON 里取 "key":"字符串"，支持 \" 转义（SSID/密码为常规字符，够用）
static bool jsonStr(const String& json, const char* key, char* out, size_t maxLen) {
  String pat = String("\"") + key + "\":\"";
  int p = json.indexOf(pat);
  if (p < 0) return false;
  p += pat.length();
  size_t i = 0;
  while (i < maxLen - 1 && p < (int)json.length()) {
    char c = json[p];
    if (c == '\\' && p + 1 < (int)json.length()) { out[i++] = json[p + 1]; p += 2; continue; }
    if (c == '"') break;
    out[i++] = c;
    p++;
  }
  out[i] = 0;
  return true;
}

// 接受 "B0-25-AA-..." / "B0:25..." / "B025AA..." 任意分隔形式
static bool parseMac(const char* s, uint8_t out[6]) {
  int digits = 0, hi = -1;
  for (const char* p = s; *p; p++) {
    int v;
    char c = *p;
    if (c >= '0' && c <= '9') v = c - '0';
    else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
    else continue;  // 跳过 -: 等分隔符
    if (hi < 0) {
      hi = v;
    } else {
      out[digits / 2] = (uint8_t)(hi * 16 + v);
      hi = -1;
      digits += 2;
      if (digits >= 12) return true;
    }
  }
  return digits == 12;
}

// ---------------------------------------------------------------- WiFi / TLS / HTTP

static bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  Serial.printf("[WiFi] 连接 %s ", cfg.ssid);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // 轮询场景关闭省电，避免射频休眠导致丢包
  WiFi.begin(cfg.ssid, cfg.pass);

  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) {
    delay(250);
    digitalWrite(LED_BUILTIN, !digitalRead(LED_BUILTIN));  // 连接中快闪
    Serial.print('.');
    yield();
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    led(false);
    Serial.printf("[WiFi] 已连接  IP: %s  RSSI: %d dBm\n",
                  WiFi.localIP().toString().c_str(), WiFi.RSSI());
  }
  return WiFi.status() == WL_CONNECTED;
}

static void setupTls() {
  // MFLN 小缓冲降低内存占用（Cloudflare 边缘支持）；
  // 若握手报错，先注释掉下一行再试
  tlsClient.setBufferSizes(512, 512);
  tlsClient.setInsecure();  // 家庭场景可接受：不校验证书，接口本身靠 DEVICE_TOKEN 鉴权
  tlsClient.setSession(&tlsSession);
}

static bool ensureTls() {
  if (tlsClient.connected()) return true;
  tlsClient.stop();  // 清理半死连接
  if (WiFi.status() != WL_CONNECTED) return false;
  bool ok = tlsClient.connect(WORKER_HOST, 443);
  if (!ok) {
    Serial.println("[TLS] 握手失败，下轮重试");
    if (++tlsFailCount >= 20) {  // WiFi 正常但连续约 1 分钟握手失败 → 重启自愈
      Serial.println("[TLS] 连续多次失败，重启自愈");
      tlsFailCount = 0;
      ESP.restart();
    }
    return false;
  }
  tlsFailCount = 0;
  return true;
}

// 发送请求并返回响应 body；网络失败或非 200 返回空串
static String httpRequest(const String& method, const String& path, const String& body) {
  if (!ensureTls()) return "";

  String req = method + " " + path + " HTTP/1.1\r\n"
               "Host: " + WORKER_HOST + "\r\n"
               "Authorization: Bearer " DEVICE_TOKEN "\r\n"
               "User-Agent: esp8266-wol/1.0\r\n"
               "Connection: keep-alive\r\n";
  if (body.length()) {
    req += "Content-Type: application/json\r\n"
           "Content-Length: " + String(body.length()) + "\r\n";
  }
  req += "\r\n" + body;

  led(true);
  tlsClient.print(req);

  // 读响应头（以空行结束）
  String headers;
  uint32_t start = millis();
  while (millis() - start < 10000) {
    while (tlsClient.available()) {
      headers += (char)tlsClient.read();
      if (headers.endsWith("\r\n\r\n")) break;
    }
    if (headers.endsWith("\r\n\r\n")) break;
    if (!tlsClient.connected() && !tlsClient.available()) break;
    delay(2);
    yield();
  }

  if (!headers.endsWith("\r\n\r\n") || !headers.startsWith("HTTP/1.")) {
    led(false);
    tlsClient.stop();
    Serial.println("[HTTP] 响应异常，重置连接");
    return "";
  }

  String statusLine = headers.substring(0, headers.indexOf('\r'));
  if (statusLine.indexOf(" 200") < 0) {
    led(false);
    tlsClient.stop();
    Serial.println("[HTTP] " + statusLine + "（检查 token / Worker 部署）");
    return "";
  }

  // 按 Content-Length 读 body
  String lower = headers;
  lower.toLowerCase();
  int len = 0;
  int p = lower.indexOf("content-length:");
  if (p >= 0) len = headers.substring(p + 15).toInt();

  String resp;
  start = millis();
  while (millis() - start < 5000) {
    while (tlsClient.available()) {
      resp += (char)tlsClient.read();
      start = millis();  // 有数据流动就续期
    }
    if (len > 0 && resp.length() >= (size_t)len) break;
    if (len == 0 && !tlsClient.connected()) break;
    delay(2);
    yield();
  }
  led(false);
  return resp;
}

// ---------------------------------------------------------------- 远程关机（电脑端代理）

// 调用电脑上常驻的关机代理（tools/pc_agent.py），由它执行 Windows 优雅关机
static void shutdownPc() {
  WiFiClient lan;
  HTTPClient http;
  http.setTimeout(5000);
  String url = String("http://") + cfg.pc_ip + ":" + AGENT_PORT + "/shutdown?token=" AGENT_TOKEN;
  if (http.begin(lan, url)) {
    int code = http.GET();
    Serial.printf("[Cmd] 关机指令 → 电脑代理 HTTP %d%s\n", code,
                  code == 200 ? "" : "（电脑不在线或代理未运行）");
    http.end();
  } else {
    Serial.println("[Cmd] 关机代理连接失败（电脑不在线或代理未运行）");
  }
}

// ---------------------------------------------------------------- WoL

// 发送 WoL 魔术包：6 字节 0xFF + 重复 16 次目标 MAC
static void sendMagicPacket() {
  uint8_t pkt[102];
  memset(pkt, 0xFF, 6);
  for (int i = 0; i < 16; i++) memcpy(pkt + 6 + i * 6, cfg.mac, 6);

  // 同时发子网广播和受限广播：部分路由器/AP 会丢弃其中一种
  IPAddress bc = WiFi.broadcastIP();
  for (int n = 0; n < 3; n++) {
    udp.beginPacket(bc, WOL_PORT);
    udp.write(pkt, sizeof(pkt));
    udp.endPacket();
    udp.beginPacket(IPAddress(255, 255, 255, 255), WOL_PORT);
    udp.write(pkt, sizeof(pkt));
    udp.endPacket();
    delay(20);
    yield();
  }
  Serial.printf("[WoL] 魔术包已发送 x3 → %s 和 255.255.255.255:%d  "
                "目标 %02X:%02X:%02X:%02X:%02X:%02X\n",
                bc.toString().c_str(), WOL_PORT,
                cfg.mac[0], cfg.mac[1], cfg.mac[2], cfg.mac[3], cfg.mac[4], cfg.mac[5]);
}

static void poll() {
  char ackedBuf[24];
  snprintf(ackedBuf, sizeof(ackedBuf), "%lld", (long long)ackedId);
  String body = httpRequest("GET", String("/api/command?acked=") + ackedBuf, "");
  if (body.length() == 0) return;  // 网络失败，静默等下轮

  int64_t id = jsonInt(body, "id");
  if (body.indexOf("\"wake\"") >= 0 && id > ackedId) {
    Serial.printf("[Cmd] 收到 wake 命令 id=%lld\n", (long long)id);
    sendMagicPacket();
    ackedId = id;
  } else if (body.indexOf("\"shutdown\"") >= 0 && id > ackedId) {
    Serial.printf("[Cmd] 收到关机命令 id=%lld\n", (long long)id);
    shutdownPc();
    ackedId = id;
  } else if (id > ackedId) {
    ackedId = id;  // 无 wake 只推进游标（例如设备重启后追上进度）
  }
}

// ---------------------------------------------------------------- 远程配置

// 心跳响应里携带 Worker 上的最新配置；rev 更新则应用并写入 EEPROM
static void applyRemoteConfig(const String& resp) {
  int64_t rev = jsonInt(resp, "rev");
  if (rev <= 0 || rev <= cfg.rev) return;

  DeviceConfig next = cfg;
  jsonStr(resp, "ssid", next.ssid, sizeof(next.ssid));
  jsonStr(resp, "pass", next.pass, sizeof(next.pass));
  char macStr[20];
  if (jsonStr(resp, "mac", macStr, sizeof(macStr)) && macStr[0]) {
    uint8_t mac[6];
    if (parseMac(macStr, mac)) memcpy(next.mac, mac, 6);
  }
  char ipStr[18];
  if (jsonStr(resp, "pc_ip", ipStr, sizeof(ipStr)) && ipStr[0]) {
    IPAddress tmp;
    if (tmp.fromString(ipStr)) strlcpy(next.pc_ip, ipStr, sizeof(next.pc_ip));
  }
  if (!next.ssid[0]) {
    Serial.println("[Cfg] 忽略空 SSID 的远程配置");
    return;
  }
  next.rev = rev;

  bool wifiChanged = strcmp(next.ssid, cfg.ssid) != 0 || strcmp(next.pass, cfg.pass) != 0;
  cfg = next;
  saveConfig();
  Serial.printf("[Cfg] 已应用远程配置 rev=%lld%s\n", (long long)rev,
                wifiChanged ? "（WiFi 变更，重连中）" : "");
  if (wifiChanged) {
    WiFi.disconnect();
    WiFi.begin(cfg.ssid, cfg.pass);
  }
}

// ---------------------------------------------------------------- OTA 远程升级

static String urlEncode(const String& s) {
  String o;
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (isalnum((unsigned char)c) || c == '-' || c == '.' || c == '_' || c == '~') o += c;
    else { char b[4]; snprintf(b, sizeof(b), "%%%02X", (unsigned char)c); o += b; }
  }
  return o;
}

// 心跳响应带 ota 字段（Worker 上固件版本比本机新）时，下载新固件自动升级。
// 成功会直接重启进新固件；失败则打印原因，继续运行当前固件。
static void checkOta(const String& resp) {
  char ver[40];
  if (!jsonStr(resp, "ota", ver, sizeof(ver)) || !ver[0] || strcmp(ver, FW_VER) == 0) return;
  Serial.printf("[OTA] 发现新固件 %s，开始升级（约 10 秒，期间设备会重启）\n", ver);
  tlsClient.stop();
  tlsClient.setBufferSizes(16384, 1024);  // OTA 临时换大缓冲：512 字节小缓冲扛不住大流量下载
  ESPhttpUpdate.rebootOnUpdate(true);  // 成功后自动重启进新固件
  ESPhttpUpdate.setClientTimeout(120000);  // 432KB 经 MFLN+TLS 下载较慢，默认 8s 读超时不够
  tlsClient.setTimeout(30000);
  String url = String("https://") + WORKER_HOST + "/api/firmware?token=" DEVICE_TOKEN +
               "&ver=" + urlEncode(FW_VER);
  t_httpUpdate_return r = ESPhttpUpdate.update(tlsClient, url, String(FW_VER));
  tlsClient.setBufferSizes(512, 512);  // 恢复小缓冲供日常轮询（成功则已重启不会走到这）
  if (r != HTTP_UPDATE_OK) {
    Serial.printf("[OTA] 升级失败 code=%d err=%d (%s)，继续运行当前固件\n",
                  (int)r, ESPhttpUpdate.getLastError(), ESPhttpUpdate.getLastErrorString().c_str());
  }
}

static void heartbeat() {
  String body = String("{\"rssi\":") + WiFi.RSSI() +
                ",\"heap\":" + ESP.getFreeHeap() +
                ",\"up\":" + millis() / 1000 + ",\"pc\":" + (pcOnline ? 1 : 0) + ",\"fw\":\"" FW_VER "\"}";
  String resp = httpRequest("POST", "/api/heartbeat", body);
  if (resp.length()) {
    Serial.println("[Hb] 心跳已上报");
    applyRemoteConfig(resp);
    checkOta(resp);
  }
}

// ---------------------------------------------------------------- 电脑开关机监测

// 状态变化才上报；上报失败不更新游标，下一轮自动重试
static void reportPcState() {
  String body = String("{\"online\":") + (pcOnline ? 1 : 0) + "}";
  if (httpRequest("POST", "/api/pcstate", body).length()) {
    lastReportedPc = pcOnline;
    Serial.printf("[PC] 状态上报: %s\n", pcOnline ? "在线" : "离线");
  }
}

static void checkPc() {
  IPAddress pcIp;
  bool ok = pcIp.fromString(cfg.pc_ip) && Ping.ping(pcIp, 2);
  if (ok) {
    pcMissCount = 0;
    pcOnline = true;
  } else if (++pcMissCount >= 2) {  // 防抖：连续 2 次 ping 失败才判离线
    pcOnline = false;
  }
  if ((int)pcOnline != lastReportedPc) reportPcState();
}

// ---------------------------------------------------------------- 应急配置热点

static String esc(const char* s) {
  String o;
  for (; *s; s++) {
    if (*s == '&') o += "&amp;";
    else if (*s == '<') o += "&lt;";
    else if (*s == '"') o += "&quot;";
    else o += *s;
  }
  return o;
}

static String macToString(const uint8_t mac[6]) {
  String s;
  for (int i = 0; i < 6; i++) {
    if (i) s += ':';
    char b[3];
    sprintf(b, "%02X", mac[i]);
    s += b;
  }
  return s;
}

static void handleApRoot() {
  String html = String("<!doctype html><html lang='zh'><head><meta charset='utf-8'>"
                  "<meta name='viewport' content='width=device-width,initial-scale=1'>"
                  "<title>WoL 配置</title><style>"
                  "body{font-family:system-ui;max-width:420px;margin:8vh auto;padding:0 16px;color:#1f2937}"
                  "input{width:100%;padding:10px;margin:4px 0 14px;box-sizing:border-box}"
                  "button{width:100%;padding:14px;background:#2563eb;color:#fff;border:0;border-radius:10px;font-size:18px}"
                  "</style></head><body><h3>WoL 唤醒器配置</h3><form method='POST' action='/save'>"
                  "WiFi 名称<input name='ssid' value='" + esc(cfg.ssid) + "'>"
                  "WiFi 密码<input name='pass' type='password' placeholder='留空保持不变'>"
                  "目标网卡 MAC<input name='mac' value='" + macToString(cfg.mac) + "'>"
                  "<button>保存并重启</button></form></body></html>");
  webServer.send(200, "text/html; charset=utf-8", html);
}

static void handleApSave() {
  String ssid = webServer.arg("ssid");
  String pass = webServer.arg("pass");
  String mac = webServer.arg("mac");
  ssid.trim();
  mac.trim();
  if (!ssid.length() || ssid.length() >= (int)sizeof(cfg.ssid)) {
    webServer.send(400, "text/plain; charset=utf-8", "SSID 不能为空且不能超过 32 字符");
    return;
  }
  uint8_t newMac[6];
  if (mac.length() && !parseMac(mac.c_str(), newMac)) {
    webServer.send(400, "text/plain; charset=utf-8", "MAC 格式无效（12 位十六进制，可带 : - 分隔符）");
    return;
  }
  strlcpy(cfg.ssid, ssid.c_str(), sizeof(cfg.ssid));
  if (pass.length()) strlcpy(cfg.pass, pass.c_str(), sizeof(cfg.pass));
  if (mac.length()) memcpy(cfg.mac, newMac, 6);
  saveConfig();
  webServer.send(200, "text/plain; charset=utf-8", "已保存，设备正在重启…");
  delay(1500);
  ESP.restart();
}

static void startApPortal() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.softAP("WoL-Setup");
  dnsServer.start(53, "*", WiFi.softAPIP());  // 劫持 DNS 做引导页
  webServer.on("/", handleApRoot);
  webServer.on("/save", HTTP_POST, handleApSave);
  webServer.onNotFound([]() {
    webServer.sendHeader("Location", "http://" + WiFi.softAPIP().toString());
    webServer.send(302, "text/plain", "");
  });
  webServer.begin();
  Serial.printf("[AP] 配置热点已开启：连接 WiFi \"WoL-Setup\"，访问 http://%s/\n",
                WiFi.softAPIP().toString().c_str());
}

// ---------------------------------------------------------------- 主流程

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  led(false);
  udp.begin(WOL_PORT);

  Serial.printf("\n[Boot] esp8266-wol 启动  编译于 %s %s\n", __DATE__, __TIME__);
  loadConfig();
  setupTls();
  bootMs = millis();
  lastHeartbeatMs = millis() - HEARTBEAT_INTERVAL_MS;  // 让首次心跳在开机后立即发生
  lastPcPingMs = millis() - PC_PING_INTERVAL_MS;       // 首次 ping 同样立即发生（先于心跳）
  ensureWiFi();
}

void loop() {
  if (apMode) {
    dnsServer.processNextRequest();
    webServer.handleClient();
    digitalWrite(LED_BUILTIN, (millis() / 500) % 2);  // 慢闪 = 热点模式
    delay(10);
    return;
  }

  uint32_t now = millis();

  // WiFi 一直连不上 → 开应急热点，避免变砖
  if (WiFi.status() != WL_CONNECTED && now - bootMs > AP_AFTER_MS) {
    startApPortal();
    return;
  }

  if (now - lastPollMs >= POLL_INTERVAL_MS) {
    lastPollMs = now;
    if (ensureWiFi()) poll();
  }
  if (now - lastPcPingMs >= PC_PING_INTERVAL_MS) {
    lastPcPingMs = now;
    if (WiFi.status() == WL_CONNECTED && cfg.pc_ip[0]) checkPc();
  }
  if (now - lastHeartbeatMs >= HEARTBEAT_INTERVAL_MS) {
    lastHeartbeatMs = now;
    if (ensureWiFi()) heartbeat();
  }
  delay(10);
}
