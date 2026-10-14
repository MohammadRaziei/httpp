# httpp — Checklist

> ⚠️ **قبل از شروع هر فاز، تصمیم‌های بخش ۷ `Architecture.md` که به آن فاز مربوط‌اند باید گرفته شده باشند.**
>
> علامت‌ها: `[x]` انجام و تأییدشده، `[ ]` انجام‌نشده. شرایط اندازه‌گیری کنار هر عدد آمده.
> **sandbox فعلی تک‌هسته است**؛ پس هر چیزی که «چندهسته» می‌گوید هنوز اندازه‌گیری نشده.

---

## وضعیت فعلی (آنچه تا امروز ساخته و تأیید شده)

- [x] موتور سرور async خودمان (Asio + llhttp): keep-alive، pipelining، chunked، محدودیت‌ها (۴۱۳/۴۱۴/۴۳۱)
- [x] سرور روی آن: router (`:name`)، فایل استاتیک با HEAD/Range، hookها، `route_async` با timeout ۵۰۴
- [x] موتور کلاینت async: TLS (mbedTLS)، pool با keep-alive، proxy (HTTP و CONNECT)، redirect، cookie/auth
- [x] لاگ داخلی (AixLog) با API در C++ و Python
- [x] `httpp run main:app`، `app.run()`، گرفتن SIGINT/SIGTERM با خاتمهٔ تمیز
- [x] `--workers N` (پردازهٔ تازه + `SO_REUSEPORT`؛ فقط Linux؛ بدون respawn)
- [x] پل async بدون GIL سمت C++ (loop inbox): `async def` از ~۱۰٫۹k به ~۱۷٫۵k رسید (تک‌هسته، handler خالی)
- [x] wheel `cp39-abi3` و `cp314-cp314t` ساخته و تست شد (هر دو حدود ۱٫۱ MB)
- [x] پایتون free-threaded: GIL بعد از import خاموش می‌ماند؛ ۱۵۵ تست + استرس‌تست پاس (**روی یک هسته**)
- [x] تست‌ها: ۱۷۹ تست C++ و ۱۵۵ تست پایتون (+۱ skip که فقط روی free-threaded معنا دارد)

---

## فاز ۰ — اندازه‌گیری مرز (اول از همه)

> هدف: فهمیدن هزینهٔ هر عبور پایتون/C++ **در binding خودمان**.

### تست‌ها

- [ ] هزینهٔ یه تماس خالی از پایتون به Cython (نیاز به یک تابع noop فقط برای بنچمارک؛ با مشورت)
  - [ ] N = 1,000,000
  - [ ] `time.perf_counter` (نه cProfile)
- [ ] (اختیاری، فقط برای مقاله) مقایسه با ctypes، cffi ABI/API، pybind11
- [ ] تعداد عبور در هر درخواست
  - [ ] شمارندهٔ اتمیک C++
  - [ ] جدا برای هر نوع:
    - [ ] ورود به handler
    - [ ] خروج از handler
    - [ ] دسترسی به state
    - [ ] validate پارامتر
    - [x] ساختن شیء پایتونی: فقط آنچه handler خواسته ساخته می‌شود (`RawRequest` مشترک با C++، `ServerRequest` تنبل)
- [ ] سهم مرز = Σ (هزینه × تعداد)
- [x] async: چند پرش داری؟ **`def`: یک پرش (Asio ← pool)؛ `async def`: یک پرش (Asio ← حلقهٔ asyncio) بدون `before_request`، دو پرش با آن.** پرش دوم حذف شد (گزینهٔ B)؛ اندازه‌گیری: `async def` از حدود ۲۲٫۵k به حدود ۵۷٫۶k رسید (یک هسته، همان اجرا؛ uvicorn حدود ۴۱k). تک‌هسته است و ادعای چندهسته‌ای نیست.

### خروجی

- [ ] جدول هزینهٔ هر نوع عبور
- [ ] جدول تعداد عبور در هر درخواست
- [ ] نمودار سهم هر مرز

---

## فاز ۱ — Baseline (تک‌هسته)

> هدف: اثبات اینکه حتی تک‌هسته‌ای بهبود داریم.

### تست‌ها

- [x] `def` handler ساده → بهتر از uvicorn؟ **بله:** ~۲۹k در برابر ~۲۴k (حدود +۲۰٪)
- [x] `async def` handler ساده → حداقل برابر؟ **نه:** ~۱۷٫۵k در برابر ~۲۴k (حدود −۲۸٪). هدف برآورده نشده.
- [ ] const handler → پایتون بیدار نشه؟ (🎯 `get_const` ساخته نشده)
- [ ] shared memory → قابل اندازه‌گیری بهتر؟ (ساخته نشده)
- [ ] profiling همزمان (perf / py-spy)

> شرایط: یک هستهٔ CPU، handler خالی، کلاینت پایتونی روی همان ماشین، ۸ اتصال، ۴ ثانیه، ۳ دور، uvicorn با httptools+uvloop.

### خروجی

- [x] جدول throughput (بالا)
- [ ] جدول latency (p50، p95، p99)
- [ ] profiling flame graph
- [ ] تحلیل: چرا بهبود داریم / نداریم. **فرضیه:** پرش اضافه بین رشته‌ها؛ `eager_task_factory` آزموده و رد شد.

---

## فاز ۱.۵ — بهبودهای سرعت (جدید؛ وابسته به تصمیم خطر ۱)

> قبل از شروع، تصمیم خطر ۱ گرفته شود.

- [ ] حذف یک پرش برای `async def` (route match روی رشتهٔ Asio وقتی `before_request` نیست)
- [ ] ساختن فقط فیلدهای لازم handler (و حفظ رفتار hookهای exception)
- [ ] `get_const` برای پاسخ ثابت
- [ ] `--workers auto` (تعداد هسته؛ روی free-threaded: یک پردازه)
- [ ] اندازه‌گیری قبل/بعد برای هر مورد با همان روش فاز ۱

---

## فاز ۲ — Multi-core

> هدف: نمودار scaling و نقطهٔ شکست. **نیاز به ماشین چندهسته‌ای.**

### محورها

- [ ] هسته: ۱، ۲، ۴، ۸، ۱۶
- [ ] handler: sync، async، const، CPU، I/O
- [ ] worker: ۱، ۴، ۸
- [ ] حالت‌ها: `cp39-abi3` تک‌پردازه، همان کد روی 3.14t، و `--workers N`
- [ ] نمودار speedup (نه throughput خام)

### خروجی

- [ ] نمودار scaling
- [ ] نقطهٔ شکست (کجا scaling می‌شکند؟)
- [ ] تحلیل: چرا؟

---

## فاز ۳ — مقایسه

> هدف: جدا کردن سهم هر مرز.

### مقایسه با

- [ ] uvicorn + FastAPI
- [ ] uvicorn + Starlette خام
- [ ] `uvicorn --workers N` (برای مقایسهٔ منصفانه با `--workers N` خودمان)
- [ ] سرور C++ خالص (بدون handler پایتون)
- [ ] httpp

### خروجی

- [ ] جدول مقایسه
- [ ] تحلیل: هر مرز چقدر هزینه دارد؟

---

## فاز ۴ — عمق

> هدف: رفتار تحت شرایط واقعی.

### محورها

- [ ] TLS: خیر، بله (**سرور هنوز TLS ندارد** 🎯؛ فقط کلاینت)
- [ ] payload: ۱۰۰B، ۱۰KB، ۱MB
- [ ] بار: بسته، باز
- [ ] latency: p50، p95، p99

### خروجی

- [ ] جدول کامل
- [ ] تحلیل: هزینهٔ mbedTLS
- [ ] تحلیل: backpressure

---

## فاز ۴.۵ — محدودیت منابع سرور (جدید؛ امنیتی، خطر ۷)

- [ ] `set_max_connections` (امروز فقط backoff وقتی fd تمام می‌شود)
- [ ] timeout هدر و محدودیت درخواست‌های کند
- [ ] timeout هر route
- [ ] تست slowloris (تا ساخته‌شدن این‌ها شکست انتظاری)
- [x] ThreadSanitizer روی موتور سرور و مسیر io: بدون گزارش
- [ ] ThreadSanitizer روی کلاینت TLS: data race داخل mbedtls (بدون `MBEDTLS_THREADING_C`) — نیاز به تصمیم
- [ ] AddressSanitizer

---

## فاز ۵ — WebSocket

> ⚠️ قبل از شروع، تصمیم backpressure (خطر ۶) گرفته شود. شروع‌نشده.

### مراحل

- [ ] Handshake (Upgrade)
- [ ] Frame parsing (text)
- [ ] Ping/Pong
- [ ] Close handshake
- [ ] Binary frames
- [ ] Fragmentation
- [ ] wss:// (نیازمند TLS سرور)
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

> ⚠️ قبل از شروع، تصمیم safety (خطر ۲) و cross-platform (خطر ۵) گرفته شود. **Linux اول.** شروع‌نشده.

### مراحل

- [ ] انتخاب پیاده‌سازی (libsharedmemory vendored، یا پیاده‌سازی خودمان پشت interface؛ خطر ۲)
- [ ] supervisor حافظه را قبل از راه‌اندازی workerها می‌سازد
- [ ] لایهٔ نازک پایتونی (proxy)
- [ ] API: `app.cache` (مقدار: `bytes`؛ شیء پایتونی با serialize صریح)
  - [ ] `__getitem__`
  - [ ] `__setitem__`
  - [ ] `__delitem__`
  - [ ] `__contains__`
- [ ] API: `app.counter`
  - [ ] `incr()`
  - [ ] `value`
- [ ] قفل بین‌پردازه‌ای robust (worker وسط قفل بمیرد)
- [ ] Cross-platform: Linux اول؛ Windows بعد
- [ ] Crash recovery
- [ ] تست چندپردازه‌ای (الگوی `tests/python/test_run.py`)

### تست‌های حیاتی

- [ ] `test_cache_shared_across_workers`
- [ ] `test_counter_atomic`
- [ ] `test_crash_recovery`
- [ ] `test_shm_linux`
- [ ] `test_shm_windows` (بعد از Linux)

---

## فاز ۷ — مقاله

> هدف: تبدیل داده به یافته.
> **نکته:** داده‌ها یکدست مثبت نیستند (`async def` امروز از uvicorn کندتر است). مقاله باید همین را صادقانه تحلیل کند،
> نه فقط نتایج خوب را نشان بدهد. هر عدد باید از اجرای واقعی و با شرایط کامل بیاید.

### محور

- [ ] حذف مرز

### مکانیزم

- [ ] profiling
- [ ] شمارندهٔ عبور
- [ ] مقایسهٔ کنترل‌شده

### داده

- [ ] بنچمارک چندهسته‌ای
- [ ] جدول مقایسه
- [ ] نمودار scaling

### نوشتن

- [ ] Abstract
- [ ] Introduction
- [ ] Related Work
- [ ] Design
- [ ] Evaluation
- [ ] Limitations (تک‌هسته، Linux، `async def`، بدون TLS سرور)

---

## فاز ۸ — لایهٔ API شبیه Starlette/FastAPI (بومی)

> ⚠️ قبل از شروع، دامنهٔ API (خطر ۴) با کاربر تصمیم‌گیری شود. **آداپتور ASGI لازم نیست.**

### مراحل (سطح Starlette)

- [ ] `Request` / `Response` (JSON، stream، redirect، کوکی) با رابط استارلت
- [ ] `Router` / `Mount` / `include_router`
- [ ] سینتکس `{id}` در مسیرها
- [ ] middleware
- [ ] فایل استاتیک (پایه‌اش در C++ هست)
- [ ] background task
- [ ] WebSocket (وابسته به فاز ۵)
- [ ] `httpp.testclient.TestClient` با رابط `TestClient` استارلت، روی سرور واقعی loopback

### مراحل (سطح FastAPI، بعد از تصمیم)

- [ ] نوع پارامتر مسیر و query از annotation (بخشی از آن امروز هست: `int`/`float` و پاسخ ۴۲۲)
- [ ] `Depends`
- [ ] مدل‌های pydantic (تصمیم: وابستگی اختیاری)
- [ ] OpenAPI

### اندازه‌گیری (از اجرای واقعی)

- [ ] اجرای تست‌های Starlette (tag مشخص، در CI کلون‌شده) با shim import و `TestClient` ما
- [ ] ثبت PASS/FAIL/SKIP و دستهٔ هر شکست
- [ ] سپس همین برای FastAPI

---

## موارد کوچک باقی‌مانده

- [ ] کامنت‌های کهنهٔ `progress.hpp` (حدود خط ۱۶ و ۴۳)
- [ ] پاسخ ۴۰۵ به‌جای ۴۰۴ برای متد اشتباه
- [ ] محافظت symlink در فایل استاتیک
- [ ] پشتیبانی از `{id}` کنار `:id` در مسیرها (شباهت ارزان به FastAPI، با مشورت)
- [ ] مثال استفاده از کلاینت TLS
- [ ] CI روی macOS و Windows (هرگز اجرا نشده)
- [ ] اجرای ماتریس `wheels.yml` در CI (فقط محلی ساخته شده)
