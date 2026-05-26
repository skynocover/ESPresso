#!/usr/bin/env python3
"""ESPresso 假資料產生器 (throwaway / Stage 2 測試用)

不取真實系統資料，只每 2 秒送一筆隨機 JSON 到板子，
用來驗證韌體的 serial 解析與 UI 更新。真正的 agent 是 agent.py。

用法:
    python3 fake_sender.py            # 自動找 /dev/cu.usbmodem*
    python3 fake_sender.py /dev/cu.usbmodemXXXX
"""
import glob
import json
import math
import sys
import time
from datetime import datetime, timedelta

import serial  # pyserial

INTERVAL_S = 2.0


def find_port(argv):
    if len(argv) > 1:
        return argv[1]
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("找不到 /dev/cu.usbmodem* — 板子接上了嗎？")
    return ports[0]


def main():
    port = find_port(sys.argv)
    print(f"開啟 {port} @ 115200，每 {INTERVAL_S}s 送假資料 (Ctrl-C 結束)")
    ser = serial.Serial(port, 115200, timeout=1)
    i = 0
    while True:
        now = datetime.now()
        # 用正弦波讓 CPU/RAM 看起來會動
        cpu = int(50 + 45 * math.sin(i / 5.0))
        ram = int(55 + 30 * math.sin(i / 11.0 + 1))
        events = [
            {"t": (now + timedelta(minutes=30)).strftime("%H:%M"), "title": "Standup"},
            {"t": (now + timedelta(hours=2)).strftime("%H:%M"), "title": "Design review"},
            {"t": (now + timedelta(hours=5)).strftime("%H:%M"), "title": "1:1 with Sam"},
        ]
        msg = {
            "time": now.strftime("%Y-%m-%d %H:%M:%S"),
            "cpu": cpu,
            "ram": ram,
            "events": events,
        }
        line = json.dumps(msg, ensure_ascii=False) + "\n"
        ser.write(line.encode("utf-8"))
        print(line.strip())
        i += 1
        time.sleep(INTERVAL_S)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n結束")
