// ioscc — clang и lld внутри процесса: компилятор для iOS, работающий на самом iOS.
// Диагностика пишется в stderr (fd 2). Вызовы не потокобезопасны: одна сборка за раз,
// на потоке со стеком от 8 МБ (clang рекурсивен).
#ifndef IOSCC_H
#define IOSCC_H
#ifdef __cplusplus
extern "C" {
#endif

// Как `clang …`, но только компиляция (-c / -S / -E / -fsyntax-only): линковку делает ioscc_ld.
// argv[0] — путь «к clang» внутри тулчейна: от него считается resource-dir (<argv0>/../../lib/clang/<ver>).
int ioscc_cc(int argc, const char **argv);

// Как `ld64.lld …` (argv[0] значения не имеет). Возвращает 0 при успехе.
// Если lld упал, повторно его вызывать нельзя до перезапуска — тогда вернётся 125.
int ioscc_ld(int argc, const char **argv);

// Версия LLVM, напр. "22.1.8".
const char *ioscc_version(void);

#ifdef __cplusplus
}
#endif
#endif
