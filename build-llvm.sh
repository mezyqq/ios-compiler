#!/usr/bin/env bash
# Сборка clang + lld как статических библиотек под iOS arm64 — компилятор для Forge.
# Запуск: ./build-llvm.sh > build.log 2>&1   (J=потоки, по умолчанию 4; IPAB_HOME — где лежит ipab с iOS SDK)
# Шаги идемпотентны: повторный запуск продолжает с места остановки.
#   1) исходники LLVM (без тестов и лишних подпроектов)
#   2) нативные инструменты сборки: llvm-tblgen, clang-tblgen, llvm-ar…
#   3) кросс-сборка библиотек clang/lld/LLVM(AArch64) для arm64-apple-ios
set -euo pipefail

C=$(cd "$(dirname "$0")" && pwd)
IPAB_HOME=${IPAB_HOME:-$HOME/ios-compiler-for-linux}   # там iOS SDK
V=22.1.8
SRC=$C/llvm-src
NAT=$C/llvm-native
OUT=$C/llvm-ios-build
SDK=$IPAB_HOME/sdks/iPhoneOS26.5.sdk
J=${J:-4}
NICE="nice -n 19"

say() { printf '\n==> [%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

# ---------------------------------------------------------------- 1. исходники
if [ ! -f "$SRC/.done" ]; then
	say "скачиваю LLVM $V"
	mkdir -p "$SRC"
	curl -fL --retry 3 -o "$C/llvm.tar.xz" \
		"https://github.com/llvm/llvm-project/releases/download/llvmorg-$V/llvm-project-$V.src.tar.xz"
	say "распаковываю (только llvm, clang, lld, libunwind, cmake, third-party)"
	P=llvm-project-$V.src
	$NICE tar xJf "$C/llvm.tar.xz" -C "$SRC" --strip-components=1 \
		--exclude="$P/llvm/test" --exclude="$P/clang/test" --exclude="$P/lld/test" \
		--exclude="$P/llvm/unittests" --exclude="$P/clang/unittests" --exclude="$P/lld/unittests" \
		--exclude="$P/llvm/docs" --exclude="$P/clang/docs" \
		"$P/llvm" "$P/clang" "$P/lld" "$P/libunwind" "$P/cmake" "$P/third-party"
	rm -f "$C/llvm.tar.xz"
	touch "$SRC/.done"
fi

# ---------------------------------------------------------------- 2. нативные инструменты
if [ ! -f "$NAT/.done" ]; then
	say "нативные инструменты"
	cmake -G Ninja -S "$SRC/llvm" -B "$NAT" \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DLLVM_USE_LINKER=lld \
		-DLLVM_ENABLE_PROJECTS="clang" -DLLVM_TARGETS_TO_BUILD=AArch64 \
		-DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
		-DLLVM_INCLUDE_DOCS=OFF -DCLANG_INCLUDE_DOCS=OFF -DCLANG_INCLUDE_TESTS=OFF \
		-DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF
	$NICE ninja -C "$NAT" -j"$J" llvm-tblgen llvm-min-tblgen clang-tblgen llvm-ar llvm-ranlib llvm-libtool-darwin llvm-nm
	touch "$NAT/.done"
fi

# ---------------------------------------------------------------- 3. кросс-сборка под iOS
mkdir -p "$OUT"
# файл тулчейна пишем один раз: его изменение заставило бы CMake переконфигурировать всё
[ -f "$OUT/ios.cmake" ] || cat > "$OUT/ios.cmake" <<EOF
set(CMAKE_SYSTEM_NAME Darwin)
set(CMAKE_SYSTEM_PROCESSOR arm64)
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET arm64-apple-ios16.0)
set(CMAKE_CXX_COMPILER_TARGET arm64-apple-ios16.0)
set(CMAKE_OSX_SYSROOT $SDK)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_AR $NAT/bin/llvm-ar)
set(CMAKE_RANLIB $NAT/bin/llvm-ranlib)
set(CMAKE_LIBTOOL $NAT/bin/llvm-libtool-darwin)
set(CMAKE_NM $NAT/bin/llvm-nm)
set(CMAKE_EXE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "-fuse-ld=lld")
set(CMAKE_FIND_ROOT_PATH $SDK)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
EOF

if [ ! -f "$OUT/build.ninja" ]; then
	say "конфигурирую кросс-сборку"
	cmake -G Ninja -S "$SRC/llvm" -B "$OUT" \
		-DCMAKE_TOOLCHAIN_FILE="$OUT/ios.cmake" \
		-DCMAKE_BUILD_TYPE=MinSizeRel \
		-DLLVM_ENABLE_PROJECTS="clang;lld" \
		-DLLVM_TARGETS_TO_BUILD=AArch64 \
		-DLLVM_HOST_TRIPLE=arm64-apple-ios16.0 \
		-DLLVM_DEFAULT_TARGET_TRIPLE=arm64-apple-ios16.0 \
		-DLLVM_NATIVE_TOOL_DIR="$NAT/bin" \
		-DLLVM_TABLEGEN="$NAT/bin/llvm-tblgen" \
		-DCLANG_TABLEGEN="$NAT/bin/clang-tblgen" \
		-DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF \
		-DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_LIBPFM=OFF -DLLVM_ENABLE_CURL=OFF -DLLVM_ENABLE_HTTPLIB=OFF \
		-DLLVM_ENABLE_BACKTRACES=OFF -DLLVM_ENABLE_CRASH_OVERRIDES=OFF \
		-DLLVM_ENABLE_PLUGINS=OFF -DLLVM_ENABLE_BINDINGS=OFF \
		-DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF \
		-DLLVM_INCLUDE_UTILS=OFF -DLLVM_INCLUDE_DOCS=OFF \
		-DLLVM_BUILD_TOOLS=OFF -DLLVM_BUILD_UTILS=OFF \
		-DCLANG_ENABLE_ARCMT=OFF -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_PLUGIN_SUPPORT=OFF \
		-DCLANG_ENABLE_OBJC_REWRITER=OFF -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF \
		-DLLD_BUILD_TOOLS=OFF
fi

say "кросс-сборка библиотек (-j$J)"
$NICE ninja -C "$OUT" -j"$J"

say "готово: $OUT/lib"
ls "$OUT/lib"/*.a | wc -l
du -sh "$OUT/lib"
