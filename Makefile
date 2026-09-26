# make            → out/libioscc.a (обёртка ioscc; сам LLVM — llvm-ios-build/lib/*.a из build-llvm.sh)
# make toolchain  → out/toolchain (заголовки clang + урезанный SDK для телефона)
IPAB_HOME ?= $(HOME)/ios-compiler-for-linux
SDK ?= $(shell ls -d $(IPAB_HOME)/sdks/iPhoneOS*.sdk | sort -V | tail -1)
B := llvm-ios-build
L := llvm-src
AR := llvm-native/bin/llvm-ar   # собран build-llvm.sh (шаг 2)
# LLVM собран без исключений и RTTI — обёртка обязана совпадать
CXXFLAGS := -target arm64-apple-ios16.0 -isysroot $(SDK) -std=c++17 -fno-exceptions -fno-rtti -Os -Wall \
	-I$(L)/llvm/include -I$(B)/include -I$(L)/clang/include -I$(B)/tools/clang/include -I$(L)/lld/include

out/libioscc.a: out/ioscc.o
	$(AR) rcs $@ $<

out/ioscc.o: ioscc.cpp ioscc.h
	@mkdir -p out
	clang++ $(CXXFLAGS) -MMD -c $< -o $@

-include out/ioscc.d

toolchain:
	./make-toolchain.sh

clean:
	rm -rf out

.PHONY: toolchain clean
