# ADR-0015: تست و CI: چه چیزی کجا اجرا می‌شود، و فهرست skipها

- **وضعیت:** پذیرفته‌شده (توصیف وضعیت فعلی؛ CI سبز است)
- **تاریخ:** 2026-10-18
- **منبع:** `.github/workflows/cmake.yml`، `wheels.yml`، `pyproject.toml`، `tests/`

## تصمیم

| کار | Linux | macOS | Windows |
|---|---|---|---|
| بیلد C++ و تست‌ها (`cmake.yml`) | بیلد + `ctest` | بیلد + `ctest` | **فقط بیلد** |
| coverage (`test_httpp_cpp`، Codecov) | بله | - | - |
| wheel + تست Python داخل wheel (`wheels.yml`) | بله | بله (universal2) | بله (AMD64 و x86) |

wheelها: `cp39` (abi3)، `cp314t`، `cp315t` با `cibuildwheel` 4.3.1 و `cp313t` با 3.4.1. تست Python با
`pytest {project}/tests` و فقط وابستگی‌های `[test]` (`pytest`، `pytest-xdist`، `pytest-cov`) اجرا می‌شود.

## xfail

هیچ تستی `xfail` ندارد. تستی که شکست می‌خورد، یا درست می‌شود یا (اگر محدودیت پلتفرم است) در جدول زیر `skip` می‌شود.

## skipها (Python)

| تست | شرط skip | کجا اجرا می‌شود |
|---|---|---|
| `test_run.py`: توقف graceful با `on_shutdown`، لاگ درخواست در CLI، supervisor با SIGTERM | `win32` (SIGTERM آنجا کشتن مستقیم است) | Linux، macOS |
| `test_run.py`: چند worker روی یک پورت (۲ تست + `workers=2` در تست import-time) | غیر Linux (`SO_REUSEPORT`) | فقط Linux |
| `test_run.py`: علامت free-threading (GIL موقع import روشن نشود) | پایتون free-threaded نباشد یا `PYTHON_GIL=1` | wheelهای `cp313t`/`cp314t`/`cp315t` |
| `test_client.py`: TLS روی سرور واقعی | ابزار `openssl` نباشد یا Windows | Linux، macOS |
| `test_embed_file.py` | `cmake` یا کامپایلر C++ در PATH نباشد | هر جا که باشند (در wheel CI معمولاً هستند) |
| `test_validate_vs_pydantic.py` (۲۴۲)، `test_models_vs_pydantic.py` (۳۶)، یک تست در `test_openapi.py` | `pydantic` نصب نباشد | **هیچ‌جای CI** |
| دو تست در `test_params_depends.py`، یک تست در `test_openapi.py` | `openapi_spec_validator` نصب نباشد | **هیچ‌جای CI** |

## C++

تست‌ها skip نمی‌شوند؛ شاخه‌های زمان کامپایل دارند: `#ifndef _WIN32` برای تست‌های سوکت خام POSIX در `test_server.cpp`
و `#ifdef _WIN32` برای ساخت پوشه‌ی lock (`_mkdir`/`_rmdir`) در `test_download.cpp`. چون Windows تست C++ اجرا نمی‌کند،
این شاخه‌ها فقط کامپایل می‌شوند.

## پیامدها

- تست‌های تفاضلی با Pydantic و اعتبارسنجی `openapi.json` با `openapi_spec_validator` **در CI اجرا نمی‌شوند**؛ این
  پوشش فقط محلی است: `pip install pydantic openapi-spec-validator && pytest tests/python` (Linux، CPython 3.12، ۱ هسته:
  **۵۰۱ قبول، ۱ skip**، که همان تست free-threading است؛ بدون این دو بسته ۲۱۹ قبول و ۷ skip). دلیل اینکه این دو بسته در
  extras تست نیستند ثبت نشده و باید با صاحب پروژه روشن شود (افزودنشان به wheel CI روی پایتون‌های free-threaded بیلد
  هر وابستگی را لازم می‌کند؛ این را نیازمند بررسی می‌دانم، نه نتیجه‌ی تست).
- هر تست جدیدی که به SIGTERM، `SO_REUSEPORT` یا `openssl` وابسته است باید همین skipها را داشته باشد.
