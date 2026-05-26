#!/usr/bin/env python3
"""ESPresso Mac 端 agent

把這台 Mac 的 CPU/RAM 使用率與 Calendar.app 即將到來的行事曆事件，
打包成 newline-delimited JSON，透過 USB serial 餵給板子 (host-link 協定)。

韌體只負責渲染；所有 OS/權限相關的複雜度都在這支 Python 裡。

協定 (每行一個 JSON 物件，\\n 結尾):
    {"time":"2026-05-26 14:29:58","cpu":42,"ram":68,
     "events":[{"t":"14:30","title":"Standup"}]}

相依: Python 3 + psutil + pyserial  (免 OAuth)
    行事曆走原生 EventKit，但透過 calbridge/ 的簽名 Swift 小程式呼叫
    (osascript whose-filter 實測 >150s 不可用；裸 Python+pyobjc 拿不到 TCC 授權)。
    記得先 `sh calbridge/build.sh` 編譯出 calbridge/eventbridge。
用法:
    python3 agent.py                       # 自動找 /dev/cu.usbmodem*
    python3 agent.py --port /dev/cu.usbmodemXXXX
"""
import argparse
import glob
import json
import os
import subprocess
import sys
import time
from datetime import datetime

import psutil  # pip install psutil
import serial  # pip install pyserial

SEND_INTERVAL_S = 2.0       # 送資料間隔 (協定預設 2s)
CAL_REFRESH_S = 300.0       # 行事曆重抓間隔 (EventKit 很快，仍快取避免每 2s 打)
CAL_HORIZON_DAYS = 7        # 往後看幾天
MAX_EVENTS = 5              # 最多送幾筆事件
RECONNECT_DELAY_S = 2.0     # 斷線後重試間隔


# ---------- CPU / RAM ----------
def sample_cpu_ram():
    """回傳 (cpu%, ram%)，皆為 0-100 整數。"""
    cpu = int(round(psutil.cpu_percent(interval=None)))
    ram = int(round(psutil.virtual_memory().percent))
    return max(0, min(100, cpu)), max(0, min(100, ram))


# ---------- Calendar via the signed Swift CalBridge helper ----------
# 原生 EventKit (predicate 查詢數毫秒)，但因 TCC 需要「有簽章 + 有用途說明」的
# 執行檔才會跳授權視窗，故包成 calbridge/eventbridge。這裡只負責呼叫它、收 JSON。
_HELPER = os.path.join(os.path.dirname(os.path.abspath(__file__)), "calbridge", "eventbridge")
_warned_no_helper = False


def fetch_events():
    """呼叫 CalBridge 取未來 CAL_HORIZON_DAYS 天、最多 MAX_EVENTS 筆事件。

    helper 不存在、未授權、無事件、或任何例外，一律回空 list 且不丟例外。
    """
    global _warned_no_helper
    if not os.path.exists(_HELPER):
        if not _warned_no_helper:
            print(f"[cal] 找不到 {_HELPER}；事件清單將為空。"
                  f"請先執行 `sh calbridge/build.sh` 編譯。", file=sys.stderr)
            _warned_no_helper = True
        return []
    try:
        proc = subprocess.run(
            [_HELPER, str(CAL_HORIZON_DAYS), str(MAX_EVENTS)],
            capture_output=True, text=True, timeout=30,
        )
        data = json.loads(proc.stdout.strip() or "[]")
    except (subprocess.TimeoutExpired, json.JSONDecodeError, OSError) as e:
        print(f"[cal] CalBridge 讀取失敗 ({e.__class__.__name__})，當作無事件", file=sys.stderr)
        return []

    if not isinstance(data, list):
        return []
    if not data:
        # 空清單常見原因：TCC 尚未授權。第一次請在自己的終端機跑一次
        # `calbridge/eventbridge` 並在跳出的視窗按「允許」。
        pass
    # 只取需要的欄位，並夾住筆數
    return [{"t": str(e.get("t", "")), "title": str(e.get("title", ""))}
            for e in data[:MAX_EVENTS] if isinstance(e, dict)]


# ---------- Serial ----------
def find_port(explicit):
    if explicit:
        return explicit
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    return ports[0] if ports else None


def open_port(explicit):
    """阻塞直到成功開啟序列埠，回傳 Serial 物件。"""
    while True:
        port = find_port(explicit)
        if port:
            try:
                ser = serial.Serial(port, 115200, timeout=1)
                print(f"[serial] 已連線 {port}")
                return ser
            except (serial.SerialException, OSError) as e:
                print(f"[serial] 開啟 {port} 失敗: {e}", file=sys.stderr)
        else:
            print("[serial] 找不到 /dev/cu.usbmodem*，等待板子接上…", file=sys.stderr)
        time.sleep(RECONNECT_DELAY_S)


# ---------- 主迴圈 ----------
def main():
    ap = argparse.ArgumentParser(description="ESPresso Mac agent")
    ap.add_argument("--port", help="序列埠 (預設自動找 /dev/cu.usbmodem*)")
    args = ap.parse_args()

    psutil.cpu_percent(interval=None)  # 第一次呼叫只是初始化基準
    ser = open_port(args.port)

    events = []
    last_cal = 0.0
    while True:
        now = time.monotonic()
        if now - last_cal >= CAL_REFRESH_S or last_cal == 0.0:
            events = fetch_events()
            last_cal = now

        cpu, ram = sample_cpu_ram()
        msg = {
            "time": datetime.now().strftime("%Y-%m-%d %H:%M:%S"),
            "cpu": cpu,
            "ram": ram,
            "events": events,
        }
        line = (json.dumps(msg, ensure_ascii=False) + "\n").encode("utf-8")

        try:
            ser.write(line)
        except (serial.SerialException, OSError) as e:
            # 板子被拔了：關掉、重連、繼續 (不退出)
            print(f"[serial] 寫入失敗 ({e})，嘗試重連…", file=sys.stderr)
            try:
                ser.close()
            except Exception:
                pass
            ser = open_port(args.port)
            continue

        time.sleep(SEND_INTERVAL_S)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n結束")
