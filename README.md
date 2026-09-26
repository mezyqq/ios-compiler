**English** · [Русский](README.ru.md)

# ios-compiler — clang and lld running on the iPhone itself

A C / Objective-C / C++ compiler for iOS that runs **on iOS** inside an app: LLVM 22 (clang + lld, AArch64 only)
is built as static libraries for `arm64-apple-ios`, and a thin wrapper, `ioscc`, calls the clang driver and
`lld::macho` in-process, with no `fork`/`exec` (iOS has neither). The output is a regular ad-hoc signed Mach-O,
ready to be packed into an `.ipa`.

Used by Forge (an IDE for iPhone). Built on Linux together with
[ipab](https://github.com/mezyqq/ios-compiler-for-linux), which provides the iOS SDK.

## Building

```sh
# requires an installed ipab (./setup.sh in it) — path in IPAB_HOME, default ~/ios-compiler-for-linux
./build-llvm.sh > build.log 2>&1   # LLVM: ~40 min native tools + ~1–2 h cross build (J=4 jobs)
make                               # out/libioscc.a — the wrapper
make toolchain                     # out/toolchain — clang headers + trimmed SDK (~160 MB, ~25 MB inside an .ipa)
```

`build-llvm.sh` can be interrupted: running it again continues where it stopped.

## Using it in an app

Link `out/libioscc.a`, all of `llvm-ios-build/lib/*.a` and `-lc++`; put `out/toolchain` into the app bundle.

```c
#include "ioscc.h"
// compile: like clang, but -c only (ioscc_ld does the linking)
const char *cc[] = { "<bundle>/toolchain/bin/clang", "-target", "arm64-apple-ios16.0",
                     "-isysroot", "<bundle>/toolchain/sdk", "-fobjc-arc", "-c", "main.m", "-o", "main.o" };
ioscc_cc(10, cc);
// link: like ld64.lld
const char *ld[] = { "ld", "-arch", "arm64", "-platform_version", "ios", "16.0", "26.5",
                     "-syslibroot", "<bundle>/toolchain/sdk", "-adhoc_codesign", "-o", "App",
                     "main.o", "availability.o", "-framework", "UIKit", "-framework", "Foundation",
                     "-lSystem", "-lobjc" };
ioscc_ld(20, ld);
```

- `argv[0]` of `ioscc_cc` is the path "to clang" inside the toolchain: clang finds its headers relative to it (`../lib/clang/22/include`).
- Diagnostics go to stderr. Call one at a time, on a thread with at least an 8 MB stack.
- A crash in cc1 or lld is caught by `CrashRecoveryContext` and returned as an error. After an lld crash it cannot
  be called again until the process restarts (`ioscc_ld` returns 125).
- Compile and link `toolchain/rt/availability.c` if the code uses `@available`.

## Limitations

- C, Objective-C, C++ (and mixes) only. Swift is not compiled on the phone.
- No assembler for `.s` files (the integrated assembler for inline asm in C works).
- No `actool`/`ibtool`: no `.xcassets`, `.storyboard`, `.xib`.

## Files

| | |
|---|---|
| `build-llvm.sh` | downloads LLVM 22.1.8, builds native tblgen, cross-builds clang/lld for iOS |
| `ioscc.h`, `ioscc.cpp` | C API: `ioscc_cc`, `ioscc_ld`, `ioscc_version` |
| `make-toolchain.sh` | `out/toolchain`: clang headers, the SDK without Swift and extras, `availability.c` from ipab |
| `Makefile` | `out/libioscc.a` and `make toolchain` |

LLVM is Apache License 2.0 with LLVM Exceptions. The iOS SDK is not part of this repository: it comes from the
installed ipab and only ends up inside the built app.
