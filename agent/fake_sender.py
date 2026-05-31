#!/usr/bin/env python3
"""ESPresso 假資料產生器 (throwaway / Stage 2 測試用)

不取真實系統資料，只每 2 秒送一筆隨機 JSON 到板子，
用來驗證韌體的 serial 解析與 UI 更新。真正的 agent 是 agent.py。

含 Claude 用量欄位 (cc_session / cc_week_pct / cc_week_reset)，方便不燒真 token
就把蕃茄鐘的用量兩行、「—」無值、DONE 成績、斷線凍結都調出來。

用法:
    python3 fake_sender.py                      # 自動找埠，預設帶用量 (session 會往上跳)
    python3 fake_sender.py /dev/cu.usbmodemXXXX
    python3 fake_sender.py --no-usage           # 完全不送 cc_* (測「—」/向前相容)
    python3 fake_sender.py --week-pct -1         # 不送本週% (測本週「—」)
    python3 fake_sender.py --week-reset ""       # 不送重置字串
    python3 fake_sender.py --session-step 0       # cc_session 凍結不動 (測斷線凍結感)
"""
import argparse
import glob
import json
import math
import random
import sys
import time
from datetime import datetime, timedelta

import serial  # pyserial


def find_port(explicit):
    if explicit:
        return explicit
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    if not ports:
        sys.exit("找不到 /dev/cu.usbmodem* — 板子接上了嗎？")
    return ports[0]


def main():
    ap = argparse.ArgumentParser(description="ESPresso 假資料產生器")
    ap.add_argument("port", nargs="?", help="序列埠 (預設自動找 /dev/cu.usbmodem*)")
    ap.add_argument("--interval", type=float, default=2.0, help="送出間隔秒 (預設 2)")
    ap.add_argument("--no-usage", action="store_true",
                    help="完全不送 cc_* 欄位 (測「—」無值 / 向前相容)")
    ap.add_argument("--week-pct", type=int, default=16,
                    help="本週配額%% (0-100)；-1 = 不送此欄 (預設 16)")
    ap.add_argument("--week-reset", default="reset in 2d 0h",
                    help="重置提示字串；空字串 = 不送此欄 (預設「reset in 2d 0h」，對齊正式 agent 英文格式)")
    ap.add_argument("--session-start", type=int, default=0, help="cc_session 起始值 (預設 0)")
    ap.add_argument("--session-step", type=int, default=-1,
                    help="每次送出 cc_session 增量；-1 = 隨機 50~500 模擬往上跳 (預設 -1)")
    args = ap.parse_args()

    port = find_port(args.port)
    print(f"開啟 {port} @ 115200，每 {args.interval}s 送假資料 (Ctrl-C 結束)")
    ser = serial.Serial(port, 115200, timeout=1)
    i = 0
    cc_session = args.session_start
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
        if not args.no_usage:
            step = random.randint(50, 500) if args.session_step < 0 else args.session_step
            cc_session += step
            msg["cc_session"] = cc_session
            if args.week_pct >= 0:
                msg["cc_week_pct"] = args.week_pct
            if args.week_reset:
                msg["cc_week_reset"] = args.week_reset
        line = json.dumps(msg, ensure_ascii=False) + "\n"
        ser.write(line.encode("utf-8"))
        print(line.strip())
        i += 1
        time.sleep(args.interval)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n結束")
