# ADR-0012: `httpp run` اولویت دارد؛ `listen`/`run` داخل فایل نادیده گرفته می‌شود، با هشدار

- **وضعیت:** پذیرفته‌شده (پیشنهاد صاحب پروژه؛ هشدار هم به درخواست او)
- **تاریخ:** 2026-10-18
- **منبع:** `src/bindings/python/httpp/server.py` (`_defer_listen`، `_skipped`)، `_run.py`، `tests/python/test_run.py`

## زمینه

فایلی که `app.listen(...)` را بدون `if __name__ == "__main__":` دارد، زیر `httpp run app:app` موقع import بلاک می‌شود:
`--port`/`--host` بی‌صدا نادیده گرفته می‌شد، `--workers 2` با خطای bind می‌مرد (کد خروج 1) و `run` هیچ‌وقت به خط
خودش نمی‌رسید.

## تصمیم

وقتی `run` یک **رشته‌ی** `"module:attr"` را import می‌کند، یک پرچم داخلی روشن است و `Server.listen()` و `Server.run()`
فقط یک هشدار روی stderr می‌نویسند و برمی‌گردند:
`httpp: ignoring app.listen('127.0.0.1', 8001): `httpp run` decides host, port and workers (call it under `if __name__ == "__main__":` ...)`.
بعد از import پرچم خاموش می‌شود و `run` با `--host/--port/--workers` خودش گوش می‌دهد. `python file.py` تغییری نکرده.

## گزینه‌های ردشده

- **کاری نکردن** (رفتار قبلی: بی‌صدا و شکننده).
- **پرتاب خطا:** فایل‌های قدیمی (۸ تا از ۱۵ مثال ریپو `listen` بدون guard دارند) زیر `httpp run` می‌شکستند.
- **نادیده گرفتن بی‌صدا:** کاربر نمی‌فهمد چرا تنظیم فایلش اعمال نشد.

## پیامدها

- فقط هدف رشته‌ای اثر دارد؛ `httpp.run(app_object)` فایلی import نمی‌کند.
- فقط `listen` و `run` رد می‌شوند؛ `listen_after_bind` و `bind_to_any_port` (برای تست) دست‌نخورده‌اند.
- پرچم یک متغیر سراسری است (`ponytail:`)؛ import هم‌زمان چند app در thread‌های مختلف در این مسیر پیش‌بینی نشده.
- روی Windows تست import-time با `workers=1` اجرا می‌شود ولی کد خروج را نمی‌سنجد (SIGTERM آنجا کشتن مستقیم است)؛
  حالت `workers=2` فقط روی Linux اجرا می‌شود (ADR-0015).

## مرتبط

ADR-0005، ADR-0011
