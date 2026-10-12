# httpp — Checklist

> ⚠️ **قبل از شروع هر فاز، تصمیم‌های بخش ۷ `Architecture.md` باید گرفته شده باشند.**

---

## فاز ۰ — اندازه‌گیری مرز (اول از همه)

> هدف: فهمیدن هزینه‌ی هر عبور پایتون/C++.

### تست‌ها

- [ ] هزینه‌ی یه تماس خالی C++ از پایتون
  - [ ] ctypes
  - [ ] cffi ABI
  - [ ] cffi API
  - [ ] C extension (METH_FASTCALL)
  - [ ] pybind11
  - [ ] N = 1,000,000
  - [ ] `time.perf_counter` (نه cProfile)
- [ ] تعداد عبور در هر درخواست
  - [ ] شمارنده اتمیک C++
  - [ ] جدا برای هر نوع:
    - [ ] ورود به handler
    - [ ] خروج از handler
    - [ ] دسترسی به state
    - [ ] validate پارامتر
    - [ ] ساختن شیء پایتونی
- [ ] سهم مرز = Σ (هزینه × تعداد)
- [ ] async: چند پرش داری؟ (Asio → pool → asyncio)

### خروجی

- [ ] جدول هزینه‌ی هر نوع عبور
- [ ] جدول تعداد عبور در هر درخواست
- [ ] نمودار سهم هر مرز

---

## فاز ۱ — Baseline (تک‌هسته)

> هدف: اثبات اینکه حتی تک‌هسته‌ای بهبود داریم.

### تست‌ها

- [ ] def handler ساده → بهتر از uvicorn؟
- [ ] async handler ساده → حداقل برابر؟
- [ ] const handler → پایتون بیدار نشه؟
- [ ] shared memory → قابل اندازه‌گیری بهتر؟
- [ ] profiling همزمان (perf / py-spy)

### خروجی

- [ ] جدول throughput
- [ ] جدول latency (p50، p95، p99)
- [ ] profiling flame graph
- [ ] تحلیل: چرا بهبود داریم / نداریم

---

## فاز ۲ — Multi-core

> هدف: نمودار scaling و نقطه‌ی شکست.

### محورها

- [ ] هسته: ۱، ۲، ۴، ۸، ۱۶
- [ ] handler: sync، async، const، CPU، I/O
- [ ] worker: ۱، ۴، ۸
- [ ] نمودار speedup (نه throughput خام)

### خروجی

- [ ] نمودار scaling
- [ ] نقطه‌ی شکست (کجا scaling می‌شکند؟)
- [ ] تحلیل: چرا؟

---

## فاز ۳ — مقایسه

> هدف: جدا کردن سهم هر مرز.

### مقایسه با

- [ ] uvicorn + FastAPI
- [ ] uvicorn + Starlette خام
- [ ] سرور C++ خالص (بدون handler پایتون)
- [ ] httpp

### خروجی

- [ ] جدول مقایسه
- [ ] تحلیل: هر مرز چقدر هزینه دارد؟

---

## فاز ۴ — عمق

> هدف: رفتار تحت شرایط واقعی.

### محورها

- [ ] TLS: خیر، بله
- [ ] payload: ۱۰۰B، ۱۰KB، ۱MB
- [ ] بار: بسته، باز
- [ ] latency: p50، p95، p99

### خروجی

- [ ] جدول کامل
- [ ] تحلیل: هزینه‌ی mbedTLS
- [ ] تحلیل: backpressure

---

## فاز ۵ — WebSocket

> ⚠️ قبل از شروع، تصمیم backpressure (خطر ۶) گرفته شود.

### مراحل

- [ ] Handshake (Upgrade)
- [ ] Frame parsing (text)
- [ ] Ping/Pong
- [ ] Close handshake
- [ ] Binary frames
- [ ] Fragmentation
- [ ] wss://
- [ ] Backpressure (فاز دوم)
- [ ] permessage-deflate (اختیاری)

### API

```python
@app.websocket("/ws")
async def ws(websocket):
    await websocket.accept()
    async for msg in websocket:
        await websocket.send(msg)
```

---

## فاز ۶ — Shared Memory

> ⚠️ قبل از شروع، تصمیم safety (خطر ۲) گرفته شود.

### مراحل

- [ ] libsharedmemory vendored
- [ ] لایه‌ی نازک پایتونی (proxy)
- [ ] API: `app.cache`
  - [ ] `__getitem__`
  - [ ] `__setitem__`
  - [ ] `__delitem__`
  - [ ] `__contains__`
- [ ] API: `app.counter`
  - [ ] `incr()`
  - [ ] `value`
- [ ] Cross-platform (Windows/Linux)
- [ ] Crash recovery
- [ ] تست چندپردازه‌ای

### تست‌های حیاتی

- [ ] `test_cache_shared_across_workers`
- [ ] `test_counter_atomic`
- [ ] `test_crash_recovery`
- [ ] `test_shm_windows`
- [ ] `test_shm_linux`

---

## فاز ۷ — مقاله

> هدف: تبدیل داده به یافته.

### محور

- [ ] حذف مرز

### مکانیزم

- [ ] profiling
- [ ] شمارنده عبور
- [ ] مقایسه کنترل‌شده

### داده

- [ ] بنچمارک چندهسته‌ای
- [ ] جدول مقایسه
- [ ] نمودار scaling

### نوشتن

- [ ] Abstract
- [ ] Introduction
- [ ] Related Work
- [ ] Design
