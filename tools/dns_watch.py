"""轮询 .work 注册局权威服务器，检测 wangshun.work 的 NS 是否切换到 Cloudflare。

用法: python tools/dns_watch.py [最大分钟数，默认8]
检测到切换输出 NS_SWITCHED 并以 0 退出；超时以 2 退出。
"""
import json
import sys
import time
import urllib.request

import dns.message
import dns.query
import dns.rdatatype

DOMAIN = "wangshun.work"
MINUTES = float(sys.argv[1]) if len(sys.argv) > 1 else 8


def doh(name, t="A"):
    url = f"https://223.5.5.5/resolve?name={name}&type={t}"
    with urllib.request.urlopen(url, timeout=10) as r:
        return json.load(r)


def tld_ip():
    hosts = [a["data"] for a in doh("work.", "NS").get("Answer", [])]
    return doh(hosts[0])["Answer"][0]["data"]


def registry_ns(ip):
    q = dns.message.make_query(DOMAIN, dns.rdatatype.NS)
    r = dns.query.tcp(q, ip, timeout=10)
    return sorted(str(x) for rr in r.answer for x in rr)


deadline = time.time() + MINUTES * 60
while time.time() < deadline:
    try:
        ip = tld_ip()
        ns = registry_ns(ip)
        status = " -> ".join(ns) if ns else "(无返回)"
        switched = any("cloudflare" in n for n in ns)
        print(time.strftime("%H:%M:%S"), status, "<- Cloudflare!" if switched else "", flush=True)
        if switched:
            print("NS_SWITCHED")
            sys.exit(0)
    except Exception as e:
        print(time.strftime("%H:%M:%S"), "查询失败:", e, flush=True)
    time.sleep(60)
print("TIMEOUT")
sys.exit(2)
