"""监听串口日志若干秒后退出（替代 pio device monitor，可用于脚本/管道）。

用法: python serial_log.py COM10 [秒数，默认30]
"""
import sys
import time

import serial

port = sys.argv[1] if len(sys.argv) > 1 else "COM10"
seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 30

s = serial.Serial(port, 115200, timeout=1)
start = time.time()
while time.time() - start < seconds:
    line = s.readline()
    if line:
        print(line.decode("utf-8", "replace"), end="", flush=True)
