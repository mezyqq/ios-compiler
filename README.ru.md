[English](README.md) · **Русский**

# ios-compiler — clang и lld, работающие на самом iPhone

Компилятор C / Objective-C / C++ для iOS, который запускается **на iOS** внутри приложения: LLVM 22 (clang + lld,
только AArch64) собирается статическими библиотеками под `arm64-apple-ios`, а тонкая обёртка `ioscc` вызывает
clang-драйвер и `lld::macho` прямо в процессе, без `fork`/`exec` (на iOS их нет). Получается обычный Mach-O
с ad-hoc подписью — из него можно собрать `.ipa`.

Используется в Forge (IDE для iPhone). Собирается на Linux вместе с
[ipab](https://github.com/mezyqq/ios-compiler-for-linux) — оттуда берётся iOS SDK.

## Сборка

```sh
# нужен установленный ipab (./setup.sh в нём) — путь задаёт IPAB_HOME, по умолчанию ~/ios-compiler-for-linux
./build-llvm.sh > build.log 2>&1   # LLVM: ~40 мин нативные инструменты + ~1–2 ч кросс-сборка (J=4 потока)
make                               # out/libioscc.a — обёртка
make toolchain                     # out/toolchain — заголовки clang + урезанный SDK (~160 МБ, ~25 МБ в .ipa)
```

`build-llvm.sh` можно прерывать: повторный запуск продолжает с места остановки.

## Подключение к приложению

Слинковать `out/libioscc.a`, все `llvm-ios-build/lib/*.a` и `-lc++`; положить `out/toolchain` в бандл.

```c
#include "ioscc.h"
// компиляция: как clang, но только -c (линковку делает ioscc_ld)
const char *cc[] = { "<бандл>/toolchain/bin/clang", "-target", "arm64-apple-ios16.0",
                     "-isysroot", "<бандл>/toolchain/sdk", "-fobjc-arc", "-c", "main.m", "-o", "main.o" };
ioscc_cc(10, cc);
// линковка: как ld64.lld
const char *ld[] = { "ld", "-arch", "arm64", "-platform_version", "ios", "16.0", "26.5",
                     "-syslibroot", "<бандл>/toolchain/sdk", "-adhoc_codesign", "-o", "App",
                     "main.o", "availability.o", "-framework", "UIKit", "-framework", "Foundation",
                     "-lSystem", "-lobjc" };
ioscc_ld(20, ld);
```

- `argv[0]` у `ioscc_cc` — путь «к clang» в тулчейне: от него clang находит свои заголовки (`../lib/clang/22/include`).
- Диагностика идёт в stderr. Вызывать по одному, на потоке со стеком от 8 МБ.
- Падение cc1 или lld перехватывается `CrashRecoveryContext` и возвращается ошибкой. После падения lld
  его нельзя вызывать до перезапуска процесса (`ioscc_ld` вернёт 125).
- `toolchain/rt/availability.c` нужно скомпилировать и слинковать, если код использует `@available`.

Помощники для редактора и JIT (всё в `ioscc.h`):

- `ioscc_check` — диагностика clang для несохранённого текста (`path:line:col: error: …`), `ioscc_complete` —
  автодополнение в строке/столбце (через таб: вид, имя, вставка с `<#заглушками#>`, подпись, тип результата),
  `ioscc_format` — clang-format со стилем из YAML `.clang-format` или стилем по умолчанию (табы).
- `ioscc_jit_load` / `ioscc_jit_lookup` / `ioscc_jit_main` / `ioscc_jit_call` — линковка объектных файлов прямо
  в текущий процесс (ORC JITLink) и запуск; `exit`/`abort`/падения возвращают управление, а не закрывают приложение.
  Селекторы и классы Objective-C регистрируются (без категорий и `+load`), статические конструкторы вызываются.
  Нужен JIT (`ioscc_jit_enabled` проверяет `CS_DEBUGGED`); символы ищутся в самом процессе.

## Ограничения

- Только C, Objective-C, C++ (и их смеси). Swift на телефоне не компилируется.
- Нет ассемблера для `.s` (встроенный ассемблер для inline asm в C работает).
- Нет `actool`/`ibtool`: без `.xcassets`, `.storyboard`, `.xib`.

## Файлы

| | |
|---|---|
| `build-llvm.sh` | скачивает LLVM 22.1.8, собирает нативные tblgen и кросс-собирает clang/lld под iOS |
| `ioscc.h`, `ioscc.cpp` | C-API: `ioscc_cc`, `ioscc_ld`, `ioscc_version`, помощники редактора (`ioscc_check`, `ioscc_complete`, `ioscc_format`), JIT (`ioscc_jit_*`) |
| `make-toolchain.sh` | `out/toolchain`: заголовки clang, SDK без Swift и лишнего, `availability.c` из ipab |
| `Makefile` | `out/libioscc.a` и `make toolchain` |

## Лицензия

ios-compiler распространяется по GNU General Public License v3.0 — см. [LICENSE](LICENSE). Сам LLVM — Apache
License 2.0 with LLVM Exceptions. iOS SDK в репозиторий не входит: он берётся из установленного ipab и попадает
только в собранное приложение.
