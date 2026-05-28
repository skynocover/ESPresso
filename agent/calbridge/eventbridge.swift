// ESPresso CalBridge — 用原生 EventKit 讀未來行事曆事件，輸出 JSON 到 stdout。
//
// 為什麼要這支 Swift 小程式：macOS 15 的 TCC 只對「有簽章 + 有用途說明字串」
// 的執行檔跳出行事曆授權視窗。裸跑的 Python 不符合，會被靜默拒絕。
// 這支編成帶 Info.plist (NSCalendars*UsageDescription) 並 ad-hoc 簽名的執行檔，
// 第一次跑會正常跳「允許」，之後 agent.py 直接呼叫它取事件。
//
// 用法：eventbridge [days] [max]   (預設 7 天、5 筆)
// 輸出：[{"t":"HH:MM","title":"..."}] 一行 JSON；任何失敗/未授權 → []

import EventKit
import Foundation

let args = CommandLine.arguments
let days = args.count > 1 ? (Int(args[1]) ?? 7) : 7
let maxN = args.count > 2 ? (Int(args[2]) ?? 5) : 5

func emit(_ events: [[String: String]]) {
    if let data = try? JSONSerialization.data(withJSONObject: events),
       let s = String(data: data, encoding: .utf8) {
        print(s)
    } else {
        print("[]")
    }
}

func collect(_ store: EKEventStore) {
    let now = Date()
    let horizon = now.addingTimeInterval(Double(days) * 86400)
    let pred = store.predicateForEvents(withStart: now, end: horizon, calendars: nil)
    let cal = Calendar.current
    let fTime = DateFormatter(); fTime.dateFormat = "HH:mm"   // 本機時區
    let fDate = DateFormatter(); fDate.dateFormat = "M/d"
    // "t" 欄位帶上日期，讓未來幾天的事件分得出是哪天 (韌體原樣渲染)：
    //   今天計時事件 → "今天 HH:mm"；其他天 → "M/d HH:mm"；全天 → "M/d"(今天→"今天")
    func label(_ ev: EKEvent) -> String {
        let d = ev.startDate!
        let day = cal.isDateInToday(d) ? "今天" : fDate.string(from: d)  // 日期前綴
        if ev.isAllDay {
            return day
        }
        return day + " " + fTime.string(from: d)
    }
    let today0 = cal.startOfDay(for: now)
    // 「還沒開始」(startDate > now) 的事件才是真正即將到來、該拿 i==0 的下一件事 accent；
    // 進行中的會議與今天的全天事件 (startDate <= now) 仍保留顯示，但排在未開始事件之後，
    // 不會因為開始時間最早 (全天=00:00) 就霸佔高亮位。
    func upcoming(_ ev: EKEvent) -> Bool { (ev.startDate ?? now) > now }
    let sorted = store.events(matching: pred)
        .filter { ev in
            guard let s = ev.startDate else { return false }
            // 用「結束時間」判斷是否還沒過，才留得住正在進行中的事件 (已開始但還沒結束)：
            //   全天事件 startDate 是當天 00:00，用「日」粒度看 endDate 是否還在今天或之後；
            //   計時事件看 endDate 是否還沒到 now (剛開始的會議不該一過 startDate 就消失)。
            return ev.isAllDay ? (ev.endDate ?? s) >= today0 : (ev.endDate ?? s) >= now
        }
        .sorted {
            let (a, b) = (upcoming($0), upcoming($1))
            if a != b { return a }               // 未開始的排前面
            return $0.startDate < $1.startDate    // 同組內依開始時間
        }
    var out: [[String: String]] = []
    for ev in sorted.prefix(maxN) {
        out.append(["t": label(ev), "title": ev.title ?? ""])
    }
    emit(out)
}

let store = EKEventStore()
let sem = DispatchSemaphore(value: 0)
var granted = false

// 授權完成回呼跑在背景佇列，故主執行緒用 semaphore 等即可，免 run loop。
if #available(macOS 14.0, *) {
    store.requestFullAccessToEvents { ok, _ in granted = ok; sem.signal() }
} else {
    store.requestAccess(to: .event) { ok, _ in granted = ok; sem.signal() }
}
sem.wait()

if granted {
    collect(store)
} else {
    emit([])  // 未授權 → 空清單，agent 照常運作
}
