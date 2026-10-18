# ADR-0006: لایه‌ی model/validation خودمان به شکل Pydantic v2، با validatorهای Cython

- **وضعیت:** پذیرفته‌شده
- **منبع:** `src/bindings/python/httpp/models.py`، `_validate.py`، `_validate_cy.pyx`، `handoff.md`

## زمینه

کد به سبک FastAPI (بدنه‌ی درخواست، response model، خطای 422) باید بدون نصب Pydantic کار کند و wheel کوچک بماند.

## تصمیم

- `httpp.models` یک زیرمجموعه‌ی هم‌شکل Pydantic v2 است: `BaseModel`، `Field`، `field_validator`/`model_validator`،
  `model_dump`، `model_validate`، `model_json_schema` و خطاها با همان شکل (`type`, `loc`, `msg`, `input`, `ctx`).
  یک model واقعی Pydantic هم هرجا model پذیرفته می‌شود کار می‌کند.
- validatorها `cdef class`های Cython هستند (`_validate_cy.pyx`). **دو گذر با یک مسیر کد:** گذر سریع بدون ساختن
  `loc` و پیام (اولین خطا برمی‌گردد)؛ فقط اگر شکست بخورد همان validator دوباره با `errors` اجرا می‌شود و خطاهای
  هم‌شکل Pydantic را می‌سازد.

## پیامدها

- یک `@field_validator` کاربر روی ورودی **نامعتبر** دو بار اجرا می‌شود، پس نباید اثر جانبی داشته باشد.
- پشتیبانی‌نشده: `validate_assignment`، computed fields، private attributes، serializer سفارشی، strict mode، generic.
- سرعت (همان ماشین، همان اجرا، ماشین نویزی است): model معتبر `User` از ۱۰٬۷۵۹ به ۳٬۳۹۸ ns؛ `Union[int,str,None]` از
  ۹۵۴ به ۱۵۹ ns؛ model نامعتبر با ۵ خطا از ۱۷٬۲۸۰ به ۱۱٬۳۰۵ ns.
- هم‌شکلی با Pydantic با ۲۷۸ تست تفاضلی سنجیده می‌شود (۲۴۲ در `test_validate_vs_pydantic.py` و ۳۶ در
  `test_models_vs_pydantic.py`). این‌ها فقط وقتی Pydantic نصب باشد اجرا می‌شوند و **در CI اجرا نمی‌شوند** (ADR-0015).
- `_validate_cy` با `freethreading_compatible=True` علامت خورده. یک تست در `test_run.py` روی wheelهای `cp313t`،
  `cp314t` و `cp315t` در CI تأیید می‌کند GIL موقع import دوباره روشن نمی‌شود؛ روی پایتون معمولی skip می‌شود.

## مرتبط

ADR-0007، ADR-0013
