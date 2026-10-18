# ADR-0010: امنیت thread در Mbed TLS: `MBEDTLS_THREADING_C` + لایه‌ی alt روی Windows

- **وضعیت:** پذیرفته‌شده
- **منبع:** `cmake/httpp_mbedtls_config.h`، `src/core/mbedtls_threading.cpp`، `src/core/mbedtls_alt/`

## زمینه

زیر ThreadSanitizer (Linux) مشخص شد `MBEDTLS_THREADING_C` خاموش بود و DRBG سراسری PSA بین handshakeهای هم‌زمان بدون
قفل مشترک بود. این فقط ناپایداری تست نبود: دو handshake می‌توانستند بایت تصادفی یکسان بگیرند.

## تصمیم

- پیکربندی از راه `TF_PSA_CRYPTO_USER_CONFIG_FILE` (سابمادول دست نمی‌خورد): `MBEDTLS_THREADING_C` با pthread روی POSIX.
- روی Windows لایه‌ی alt روی `SRWLOCK`/`CONDITION_VARIABLE` که موقع بار شدن کتابخانه ثبت می‌شود.
  `-DHTTPP_MBEDTLS_THREADING_ALT=ON` همین لایه را روی POSIX هم فعال می‌کند تا زیر TSAN تست شود.
- `MBEDTLS_SELF_TEST` خاموش شد: شمارنده‌های `add_count/dbl_count/mul_count` در `ecp.c` فقط برای self-test هستند و
  بدون قفل افزایش می‌یابند (race طراحی‌شده).

## پیامدها

- زیر TSAN با pthread و با alt، ۸۶ تست `async_*` قبول و race مربوط به mbedtls نیست؛ `many_concurrent_handshakes`
  روی Linux ۳۰ بار پشت هم بدون شکست.
- `mbedtls_threading_free_alt()` هیچ‌وقت صدا زده نمی‌شود (`ponytail:`؛ ارتقا: shutdown صریح بعد از join threadهای TLS).
- CI: تست‌های C++ (شامل `many_concurrent_handshakes`) روی Linux و macOS اجرا و سبز می‌شوند. روی Windows لایه‌ی SRWLOCK
  در هر wheel بیلد می‌شود و تست‌های Python wheel سبزند، ولی تست‌های C++ روی Windows اجرا نمی‌شوند و تست‌های TLS پایتون
  (`needs_openssl`) روی Windows skip می‌شوند (ADR-0015).

## مرتبط

ADR-0003
