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
傳輸 (兩種，協定完全相同；資料先打包成同一份 newline-JSON):
    - serial：開 /dev/cu.usbmodem* 寫入
    - network：解析 espresso.local (或 --host)，開 TCP 寫入；只用標準函式庫 socket
用法:
    python3 agent.py                       # 自動探索：能解析 espresso.local 就走網路，否則走序列
    python3 agent.py --net                 # 強制網路 (espresso.local:3333)
    python3 agent.py --net --host 192.168.1.42   # mDNS 不通時手動指定 IP
    python3 agent.py --serial              # 強制序列，自動找 /dev/cu.usbmodem*
    python3 agent.py --serial --port /dev/cu.usbmodemXXXX
"""
import argparse
import glob
import json
import os
import socket
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

DEFAULT_HOST = "espresso.local"  # 韌體 mDNS 宣告的名稱
DEFAULT_TCP_PORT = 3333          # 韌體 TCP 線路埠 (與 main.cpp 的 TCP_PORT 一致)


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


# ---------- 傳輸 ----------
# 兩種傳輸都提供同樣的介面：send(line_bytes) 失敗時丟例外；主迴圈接住後呼叫
# reconnect() 重連、不退出。資料打包邏輯與兩者無關 (協定相同)。

class Transport:
    """共用的重連流程：關掉舊的連線 (吞掉清理時的例外) 後重新 connect()。
    子類需提供 name、connect()、send() 與 _close() (關閉底層連線)。"""

    def reconnect(self):
        try:
            self._close()
        except Exception:
            pass
        self.connect()


class SerialTransport(Transport):
    name = "serial"

    def __init__(self, explicit_port):
        self._explicit = explicit_port
        self._ser = None

    @staticmethod
    def _find_port(explicit):
        if explicit:
            return explicit
        ports = sorted(glob.glob("/dev/cu.usbmodem*"))
        return ports[0] if ports else None

    def connect(self):
        """阻塞直到成功開啟序列埠。"""
        while True:
            port = self._find_port(self._explicit)
            if port:
                try:
                    self._ser = serial.Serial(port, 115200, timeout=1)
                    print(f"[serial] 已連線 {port}")
                    return
                except (serial.SerialException, OSError) as e:
                    print(f"[serial] 開啟 {port} 失敗: {e}", file=sys.stderr)
            else:
                print("[serial] 找不到 /dev/cu.usbmodem*，等待板子接上…", file=sys.stderr)
            time.sleep(RECONNECT_DELAY_S)

    def send(self, data):
        self._ser.write(data)

    def _close(self):
        if self._ser:
            self._ser.close()
        self._ser = None


class NetworkTransport(Transport):
    name = "network"

    def __init__(self, host, port):
        self._host = host
        self._port = port
        self._sock = None

    def connect(self):
        """阻塞直到成功連上 TCP；解析 (mDNS) 失敗或拒連都重試、不退出。"""
        while True:
            try:
                self._sock = socket.create_connection((self._host, self._port), timeout=5)
                self._sock.settimeout(5)
                print(f"[net] 已連線 {self._host}:{self._port}")
                return
            except (socket.gaierror, OSError) as e:
                print(f"[net] 連線 {self._host}:{self._port} 失敗 ({e})，重試…",
                      file=sys.stderr)
            time.sleep(RECONNECT_DELAY_S)

    def send(self, data):
        self._sock.sendall(data)

    def _close(self):
        if self._sock:
            self._sock.close()
        self._sock = None


def _board_reachable(host, port, timeout=2.0):
    """auto 模式用：實際試開一次 TCP (短逾時) 判斷板子在不在網路上，連完即關。
    只看名稱能否解析不夠——mDNS 殘留/別台主機應答時名稱解析得到但板子離線，
    會讓 NetworkTransport.connect() 無限重連卡死、不退回插著的序列埠。"""
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def select_transport(args):
    """依旗標挑傳輸；預設自動探索：TCP 連得上 espresso.local 走網路，否則走序列。"""
    if args.net:
        return NetworkTransport(args.host, args.tcp_port)
    if args.serial:
        return SerialTransport(args.port)
    # auto：實際試連 TCP (不只看名稱解析) 判斷板子在不在線，連得上走網路、否則退回序列
    if _board_reachable(args.host, args.tcp_port):
        print(f"[auto] {args.host}:{args.tcp_port} 連得上 → 走網路")
        return NetworkTransport(args.host, args.tcp_port)
    print(f"[auto] {args.host}:{args.tcp_port} 連不上 → 走序列")
    return SerialTransport(args.port)


# ---------- 主迴圈 ----------
def main():
    ap = argparse.ArgumentParser(description="ESPresso Mac agent")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--net", action="store_true",
                      help="強制走網路 (TCP 到 espresso.local)")
    mode.add_argument("--serial", action="store_true",
                      help="強制走序列 (/dev/cu.usbmodem*)")
    ap.add_argument("--host", default=DEFAULT_HOST,
                    help=f"網路模式的主機名/IP (預設 {DEFAULT_HOST}；mDNS 不通時填 IP)")
    ap.add_argument("--tcp-port", type=int, default=DEFAULT_TCP_PORT,
                    help=f"網路模式的 TCP 埠 (預設 {DEFAULT_TCP_PORT})")
    ap.add_argument("--port", help="序列模式的埠 (預設自動找 /dev/cu.usbmodem*)")
    args = ap.parse_args()

    psutil.cpu_percent(interval=None)  # 第一次呼叫只是初始化基準
    transport = select_transport(args)
    transport.connect()

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
            transport.send(line)
        except (serial.SerialException, OSError) as e:
            # 板子重開/WiFi 掉/Mac 睡醒/被拔線：重連、繼續 (不退出)
            print(f"[{transport.name}] 寫入失敗 ({e})，嘗試重連…", file=sys.stderr)
            transport.reconnect()
            continue

        time.sleep(SEND_INTERVAL_S)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        print("\n結束")
