# ADR-0003: موتور شبکه = Asio + llhttp + Mbed TLS؛ cpp-httplib فقط برای کلاینت sync

- **وضعیت:** پذیرفته‌شده
- **تصمیم‌گیرنده:** صاحب پروژه
- **منبع:** `Architecture.md` بخش ۳ و ۶، `src/third_party/README.md`

## زمینه

باید parser، event loop و TLS انتخاب شوند، بدون dependency سیستمی و با wheel کوچک.

## تصمیم

| نقش | انتخاب | شکل vendoring |
|---|---|---|
| parser HTTP/1.1 (سرور و کلاینت) | llhttp | بدون تغییر در `src/llhttp` |
| event loop و I/O | Asio standalone | git submodule |
| TLS (کلاینت) | Mbed TLS | git submodule |
| URL | liburlparser | git submodule |
| لاگ داخلی | AixLog v1.5.0 | یک فایل، یک patch علامت‌گذاری‌شده |
| کلاینت **sync**، `curl.cpp`، `download.cpp` | cpp-httplib v0.54.1 | `httplib.h` **دست‌نخورده** |

سرور و کلاینت async روی cpp-httplib نیستند. cpp-httplib قرار است بازنشسته شود وقتی کلاینت sync هم مال ما شد.
patch AixLog فقط این است که `std::clog` کل پروسه را عوض نکند (یک کتابخانه نباید این کار را بکند).

## گزینه‌های ردشده

OpenSSL (سنگین)، libuv (dependency خارجی)، h11 (پایتون، کند). تفاوت سرعت با uvicorn از parser نمی‌آید، چون
`httptools` هم روی llhttp است.

## پیامدها

- `httplib.h` را هیچ‌وقت patch نمی‌کنیم؛ هر تغییر vendor باید در `src/third_party/README.md` ثبت شود.
- سرور هنوز **TLS ندارد**. فقط کلاینت HTTPS می‌زند.
- HTTP/2، HTTP/3، gRPC و SOCKS proxy هدف نیستند.

## مرتبط

ADR-0004، ADR-0010
