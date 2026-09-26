#!/usr/bin/env bash
# make-toolchain.sh — собирает out/toolchain: всё, что нужно ioscc на телефоне, кроме самого кода.
#   bin/                    пусто: argv[0]=<toolchain>/bin/clang, от него clang находит resource-dir
#   lib/clang/<N>/include   встроенные заголовки clang (stddef.h, arm_neon.h…)
#   sdk/                    урезанный iOS SDK: заголовки, modulemap, apinotes, .tbd — без Swift
#   rt/availability.c       замена compiler-rt для @available (из ipab)
# Повторный запуск пересобирает out/toolchain целиком (~1 мин).
set -euo pipefail
C=$(cd "$(dirname "$0")" && pwd)
IPAB_HOME=${IPAB_HOME:-$HOME/ios-compiler-for-linux}
SDK=${SDK:-$(ls -d "$IPAB_HOME"/sdks/iPhoneOS*.sdk | sort -V | tail -1)}
LLVM_BUILD=${LLVM_BUILD:-$C/llvm-ios-build}
T=$C/out/toolchain
say() { printf '==> %s\n' "$*"; }

rm -rf "$T.tmp"; mkdir -p "$T.tmp/bin" "$T.tmp/rt" "$T.tmp/sdk"

# ---- заголовки clang: из нашей сборки LLVM, иначе из системного clang той же major-версии
V=$(sed -n 's/.*LLVM_VERSION_MAJOR \([0-9]*\).*/\1/p' "$LLVM_BUILD/include/llvm/Config/llvm-config.h")
R=$LLVM_BUILD/lib/clang/$V/include
if [ ! -f "$R/arm_neon.h" ]; then
	R=$(clang -print-resource-dir)/include
	[ "$(basename "$(dirname "$R")")" = "$V" ] || { echo "нет заголовков clang $V" >&2; exit 1; }
fi
say "заголовки clang $V ← $R"
mkdir -p "$T.tmp/lib/clang/$V"
# x86/ppc/riscv/… интринсики на arm64 не нужны
(cd "$R" && find . -type f ! -name '*intrin*.h' ! -name 'avx*' ! -name 'amx*' ! -name '*mmintrin*' \
	! -name 'riscv_*' ! -name 'altivec.h' ! -name 'htm*' ! -name 'hexagon_*' ! -name 'hvx_*' \
	! -name 'velintrin*' ! -name 'wasm_*' ! -name 'lasx*' ! -name 'lsx*' ! -name 'larchintrin.h' \
	-print0 | cpio -0 -pdm --quiet "$T.tmp/lib/clang/$V/include")
# arm-интринсики с «intrin» в имени нужны
for f in arm_acle.h arm_cmse.h arm64intr.h armintr.h; do [ -f "$R/$f" ] && cp "$R/$f" "$T.tmp/lib/clang/$V/include/"; done

# ---- SDK
say "SDK ← $SDK"
S=$T.tmp/sdk
cp "$SDK/SDKSettings.json" "$S/" 2>/dev/null || true
[ -f "$SDK/SDKSettings.plist" ] && cp "$SDK/SDKSettings.plist" "$S/"
(cd "$SDK" && {
	find usr/include \( -type f -o -type l \) -print0
	find usr/lib -maxdepth 2 -name '*.tbd' \( -type f -o -type l \) -print0
	# фреймворки (и SubFrameworks из iOS 26): заголовки, modulemap, apinotes, .tbd — без swiftmodule/swiftinterface
	find System/Library/Frameworks System/Library/SubFrameworks \( -name '*.swiftmodule' -o -name '*.swiftinterface' -o -name '*.swiftdoc' \) -prune \
		-o \( -type f -o -type l \) \( -path '*/Headers/*' -o -name '*.modulemap' -o -name '*.tbd' -o -name '*.apinotes' \) -print0
} | cpio -0 -pdm --quiet "$S")
find "$S" -xtype l -delete   # битые симлинки (цель не попала в выборку)

# приватные .tbd, на которые ссылаются уже взятые (reexport), — транзитивно
n=0
while :; do
	new=0
	while IFS= read -r p; do
		f=${p#/}; d=$(dirname "$f")
		t=$f.tbd; [ -f "$SDK/$t" ] || t=$d/$(basename "$f").tbd
		[ -f "$SDK/$t" ] && [ ! -f "$S/$t" ] || continue
		mkdir -p "$S/$(dirname "$t")"; cp "$SDK/$t" "$S/$t"; new=1; n=$((n + 1))
	done < <(grep -rhoE "'?/System/Library/PrivateFrameworks/[^' ,]+" "$S" --include='*.tbd' | tr -d "'" | sort -u)
	[ $new = 1 ] || break
done
say "приватных .tbd по реэкспортам: $n"

cp "$IPAB_HOME/runtime/availability.c" "$T.tmp/rt/"
rm -rf "$T"; mv "$T.tmp" "$T"
say "готово: $T ($(du -sh "$T" | cut -f1))"
