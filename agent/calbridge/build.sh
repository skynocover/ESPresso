#!/bin/sh
# 編譯 + 簽名 ESPresso CalBridge 行事曆讀取器。
# 需要 Xcode command line tools (swiftc)。產出: ./eventbridge
#
# 關鍵：把 Info.plist 嵌進 Mach-O 的 __TEXT,__info_plist 區段，並 ad-hoc 簽名，
# TCC 才認得用途說明字串、第一次執行才會跳出行事曆授權視窗。
set -e
cd "$(dirname "$0")"

swiftc -O eventbridge.swift -o eventbridge \
    -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __info_plist -Xlinker Info.plist

# ad-hoc 簽名 (穩定 identifier；重新編譯後 cdhash 會變，需重新授權一次)
codesign --force --sign - --identifier com.espresso.calbridge eventbridge

echo "[build] 完成: $(pwd)/eventbridge"
echo "[build] 第一次手動跑一次以觸發授權: ./eventbridge"
