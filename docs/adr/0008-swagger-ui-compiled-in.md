# ADR-0008: Swagger UI داخل خود کتابخانه کامپایل می‌شود (بدون CDN و بدون فایل داده)

- **وضعیت:** پذیرفته‌شده
- **منبع:** `cmake/EmbedFile.cmake`، `src/core/swagger_ui.cpp`، `src/third_party/swagger-ui/VENDORED.md`

## زمینه

`/docs` باید آفلاین کار کند (بدون CDN) و بسته‌ی نصب‌شده نباید پوشه‌ی فایل `.js/.css/.png` داشته باشد.

## تصمیم

- Swagger UI 5.33.x **vendored** در `src/third_party/swagger-ui/` (Apache-2.0).
- `embed_files(...)` در configure هر فایل را به **یک** هدر تولیدشده تبدیل می‌کند: `inline constexpr unsigned char n[]`
  در `.rodata`، یک literal به ازای هر **۳۲ بایت**، هر بایت به شکل `\xNN` (باندل JS یک خط ۱٫۵ مگابایتی است و
  PNG بایت NUL دارد). هدر فقط وقتی ورودی عوض شود دوباره نوشته می‌شود (`configure_file`)، و `CMAKE_CONFIGURE_DEPENDS`
  تغییر منبع را می‌گیرد.
- C++: `httpp::find_swagger_ui_asset(name)` و `httpp::mount_swagger_ui(server&, docs_path, openapi_url, title)`.
  پایتون: `Server.mount_swagger_ui` (صفحه و سه فایل از C++ و بدون GIL سرو می‌شوند).

## گزینه‌های ردشده

CDN (آفلاین نمی‌شود)، پوشه‌ی داده کنار wheel، `inline const std::string` (هر پروسه ۱٫۶ MB را روی heap کپی می‌کرد).
ReDoc اضافه نشد (باندل دوم ~۱ MB).

## پیامدها

- هر درخواست بدنه را کپی می‌کند (`ponytail:`؛ مسیر ارتقا: بدنه‌ی `string_view` ساکن توی `response`).
- هشدار `-Woverlength-strings` روی literalهای چند مگابایتی (خطا نیست؛ پیشنهاد: `HTTPP_EMBED_AS_ARRAY` پیش‌فرض).
- `/docs` فقط در Chromium دیده شده.
- ارزیابی‌شده و **تصمیم گرفته نشده:** جایگزین سبک‌تر. اندازه‌ی فایل باندل (خام / gzip -9): Swagger 1550 / 420 KB
  (+ CSS 181 KB)؛ RapiDoc 524 / 135 KB؛ Stoplight Elements 2035 / 599 KB؛ Redoc 2236 / 634 KB؛ Scalar 4512 / 1303 KB.
  سازگاری RapiDoc با `openapi.json` فعلی و ظاهرش امتحان نشده.

## مرتبط

ADR-0007
