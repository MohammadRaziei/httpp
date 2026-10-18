# Architecture Decision Records

هر فایل یک تصمیم معماری را با **زمینه، تصمیم، گزینه‌های ردشده و پیامدها** ثبت می‌کند. این‌ها از `docs/Architecture.md`،
`handoff.md` و کد واقعی استخراج شده‌اند؛ هرجا چیزی هنوز اجرا یا تأیید نشده، در همان ADR نوشته شده است.

| ADR | تصمیم | وضعیت |
|---|---|---|
| [0001](0001-own-both-layers.md) | httpp هر دو لایه را مالک می‌شود (جای FastAPI/Starlette و uvicorn) | پذیرفته‌شده |
| [0002](0002-standard-asyncio-loop.md) | handlerهای `async def` روی حلقه‌ی asyncio استاندارد | پذیرفته‌شده |
| [0003](0003-engine-and-third-party.md) | Asio + llhttp + Mbed TLS؛ cpp-httplib فقط کلاینت sync | پذیرفته‌شده |
| [0004](0004-zero-system-dependencies.md) | بدون وابستگی سیستمی | پذیرفته‌شده |
| [0005](0005-multiprocess-workers.md) | پردازه‌ی تازه + `SO_REUSEPORT` (Linux) | پذیرفته‌شده |
| [0006](0006-own-model-layer.md) | لایه‌ی model شبیه Pydantic v2 با validator Cython | پذیرفته‌شده |
| [0007](0007-openapi-from-one-plan.md) | OpenAPI از همان plan درخواست | پذیرفته‌شده |
| [0008](0008-swagger-ui-compiled-in.md) | Swagger UI داخل کتابخانه کامپایل می‌شود | پذیرفته‌شده |
| [0009](0009-hot-path.md) | مسیر داغ: dispatch روی thread I/O، یک تایمر، router با index | پذیرفته‌شده |
| [0010](0010-mbedtls-threading.md) | امنیت thread در Mbed TLS | پذیرفته‌شده |
| [0011](0011-logging.md) | لاگ درخواست و آدرس بایند در C++؛ پیش‌فرض‌ها | پذیرفته‌شده |
| [0012](0012-run-wins-over-listen.md) | `httpp run` روی `listen` داخل فایل اولویت دارد | پذیرفته‌شده |
| [0013](0013-packaging.md) | `cp39-abi3` + wheel جدا برای free-threaded | پذیرفته‌شده |
| [0014](0014-coding-conventions.md) | `snake_case` به سبک STL، سبک ponytail | پذیرفته‌شده |
| [0015](0015-testing-and-ci.md) | چه چیزی کجا تست می‌شود؛ فهرست skipها | پذیرفته‌شده |

## هنوز تصمیم گرفته نشده

اینجا ADR ندارند، چون تصمیم نیستند؛ در `docs/Architecture.md` بخش ۷ پیگیری می‌شوند و باید **با صاحب پروژه** گرفته شوند:
کتابخانه‌ی shared memory و دامنه‌ی `app.shared`، سطح coupling، دامنه‌ی دقیق API (Starlette-like یا FastAPI-like هم)،
حذف پرش در مسیر `async def`، backpressure در WebSocket، آزمایش sub-interpreter، جایگزین سبک‌تر برای Swagger UI.

## نوشتن ADR تازه

نام فایل `NNNN-کوتاه-انگلیسی.md`. بخش‌ها: وضعیت، زمینه، تصمیم، گزینه‌های ردشده، پیامدها (همراه با آنچه تأیید **نشده**)،
مرتبط. ADR پذیرفته‌شده را ویرایش نکنید؛ تصمیم عوض شد، ADR جدید بنویسید و وضعیت قدیمی را «جایگزین‌شده با ADR-NNNN» کنید.
