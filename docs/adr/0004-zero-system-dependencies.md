# ADR-0004: بدون وابستگی سیستمی؛ هر وابستگی vendored یا submodule

- **وضعیت:** پذیرفته‌شده
- **تصمیم‌گیرنده:** صاحب پروژه
- **منبع:** `handoff.md` بخش ۱، `Architecture.md` هدف ۴

## تصمیم

هیچ چیز با `apt`/`brew`/بسته‌ی سیستمی نصب نمی‌شود. هر وابستگی جدید یا فایل vendored است یا git submodule. وابستگی‌های
pip فقط برای **زمان بیلد** مجازند (`scikit-build-core`، `cython>=3.1`، `cython-cmake`، `jsonschema`، `jinja2`؛ در `pyproject.toml`).
در زمان اجرا wheel هیچ وابستگی ندارد.

## پیامدها

- بیلد از صفر روی ۱ هسته حدود ۵ دقیقه (Python) و ۵ دقیقه (تست‌های C++ با Mbed TLS کامل) است.
- configure فایل‌های منبع tf-psa-crypto را با اسکریپت‌های Python خودش تولید می‌کند و به `jsonschema` و `jinja2`
  نیاز دارد (بدون آن‌ها configure با خطای `Failed to generate tf-psa-crypto sources` تمام می‌شود؛ همین را روی
  یک ماشین بدون آن‌ها دیدم).
- configure یک‌بار `public_suffix_list.dat` را دانلود می‌کند.

## مرتبط

ADR-0003، ADR-0013
