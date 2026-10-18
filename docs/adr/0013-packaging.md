# ADR-0013: بسته‌بندی: `cp39-abi3` + wheel جدا برای هر نسخه‌ی free-threaded

- **وضعیت:** پذیرفته‌شده
- **منبع:** `Architecture.md` اصل ۵، `.github/workflows/wheels.yml`، `handoff.md`

## تصمیم

- یک wheel `cp39-abi3` برای پایتون‌های معمولی (Cython با `Py_LIMITED_API=0x030900f0`).
- پایتون free-threaded (3.13t، 3.14t، ...) stable ABI ندارد؛ برای هر نسخه wheel جدا (`cp313t`، `cp314t`، `cp315t`).
  `abi3t` از CPython 3.15 می‌آید.
- «یک wheel برای همه‌جا» درست نیست: برای هر سیستم‌عامل و معماری یک wheel لازم است.
- `PIP_PREFER_BINARY=1` در CI تا `rpds-py` (وابستگی `jsonschema`) از سورس با Rust ساخته نشود.
- CI: `cibuildwheel` 4.3.1 برای `cp39` (abi3)، `cp314t` و `cp315t` روی Linux، macOS (universal2) و Windows (AMD64 و x86)؛
  `cp313t` با یک مرحله‌ی جدا از `cibuildwheel` 3.4.1، چون سری 4.x پایتون 3.13 free-threaded را حذف کرد. هر wheel با
  `pytest {project}/tests` تست می‌شود (extras: `pytest`، `pytest-xdist`، `pytest-cov`) و همه سبزند.

## پیامدها

- حجم: سند معماری «حدود ۱٫۱ MB» می‌گوید، ولی wheel منتشرشده‌ی 0.7.0 روی PyPI (Linux، cp312) با `pip-size`
  **۱٫۸ MB** است و وابستگی runtime ندارد؛ سند قدیمی است.
- CI روی Windows تست C++ اجرا نمی‌کند (فقط build)؛ تست‌های Python داخل wheel روی Windows اجرا می‌شوند.

## مرتبط

ADR-0004، ADR-0006
