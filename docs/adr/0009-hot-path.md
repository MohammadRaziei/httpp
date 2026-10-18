# ADR-0009: مسیر داغ: dispatch روی thread ورودی/خروجی، یک تایمر برای همه، router با index

- **وضعیت:** پذیرفته‌شده
- **منبع:** `handoff.md` («hot path rebuilt»)، `src/core/server.cpp`

## تصمیم

- `server::route_io(method, path, io_handler)` handler را **روی thread I/O** اجرا می‌کند (باید فوری برگردد و بلاک
  نکند). `route_shared` یک route عادی روی worker pool است که `shared_ptr<const request>` می‌گیرد. وقتی app route
  `io` دارد و `before` hook ندارد، درخواست همان‌جا parse و match می‌شود؛ بقیه parse‌شده به pool می‌روند.
- **یک تایمر** برای همه‌ی درخواست‌های منتظر پاسخ (به‌جای یک تایمر 504 برای هر درخواست): نقشه‌ی مرتب بر اساس
  رسیدن و یک sweep. تایمر هر درخواست حدود ۱/۳ زمان فعال thread I/O بود؛ بدون آن `async` از ۱۳k به ۳۳k رسید.
- **router با index:** routeهای فقط-literal با یک جست‌وجوی hash؛ بقیه فقط بین routeهای پارامتری با همان متد و همان
  تعداد segment، همیشه «اولین route به ترتیب ثبت که می‌خورد». با ۳۰۰ route و درخواست به آخری: +۲۰٪ (`def`) و +۲۶٪ (`async`).
- پایتون: `RawRequest` (Cython، مشترک با C++، فیلدها به‌صورت تنبل)، `ServerRequest` فقط اگر handler `request` بخواهد.

## پیامدها

- مسیر `async def` یک پرش بین thread‌ها دارد؛ `def` هم یک پرش (به pool، چون ممکن است بلاک کند).
- ارقام (۱ هسته، سرور و `oha` روی یک هسته، ۳۲ اتصال، میانه؛ فقط نسبت‌های همان اجرا معنی دارند):
  uvicorn 43.2k، httpp `def` 62.8k، `async def` 64.4k روی `hello`. ماشین ۱۰–۳۰٪ نوسان داشت.
  در بنچمارک بعدی (جلسه‌ی اخیر) روی `item`: uvicorn 21.1k، `def` 25.2k، `async def` 24.9k.
  **عدد چندهسته‌ای وجود ندارد.**
- تایمر 504 باید قبل از `listen` تنظیم شود (فرض ترتیب‌دار بودن deadlineها).
- در `def`، هزینه‌ی پرش به pool باقی است؛ ارزان‌تر کردنش سؤال باز است.

## مرتبط

ADR-0002، ADR-0005
