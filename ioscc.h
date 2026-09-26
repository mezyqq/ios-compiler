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

// ── Редактор: разбор файла из памяти (text — несохранённое содержимое path). argv — как у ioscc_cc,
// но без -c/-o и без имени файла. Результаты — malloc-строки (освобождать free), NULL при сбое.

// Диагностика clang: строки «path:line:col: error: сообщение».
char *ioscc_check(int argc, const char **argv, const char *path, const char *text);

// Автодополнение в позиции line:col (с 1). Строка на вариант, поля через таб:
// вид, имя, вставка (параметры как <#int x#>), подпись, тип результата. Не больше max вариантов.
char *ioscc_complete(int argc, const char **argv, const char *path, const char *text, int line, int col, int max);

// clang-format. style — YAML (.clang-format) или NULL: стиль Forge (табы, без ограничения ширины).
char *ioscc_format(const char *code, const char *path, const char *style);

// ── JIT: объектные файлы линкуются прямо в память Forge (ORC/JITLink), без установки .ipa.
// Нужен включённый JIT (процесс под отладчиком). Символы берутся из процесса Forge.
// Регистрируются ObjC-селекторы и классы (без категорий), вызываются статические конструкторы.
typedef struct ioscc_jit ioscc_jit;

// Включён ли JIT (CS_DEBUGGED).
int ioscc_jit_enabled(void);

// Линкует n объектных файлов. Ошибки — в stderr, тогда NULL. Сессия живёт до конца процесса.
ioscc_jit *ioscc_jit_load(int n, const char **objects);

// Адрес символа (имя без «_») или NULL.
void *ioscc_jit_lookup(ioscc_jit *jit, const char *name);

// Вызывает main(argc, argv) на текущем потоке. exit()/abort()/падение/ioscc_jit_stop возвращают управление:
// код выхода, 128+сигнал при падении.
int ioscc_jit_main(ioscc_jit *jit, int argc, const char **argv);

// Вызывает `void *name(void)` с той же защитой от падений. *status: 0 — ок, -1 — нет символа, иначе код.
void *ioscc_jit_call(ioscc_jit *jit, const char *name, int *status);

// Прерывает ioscc_jit_main на его потоке (сигналом).
void ioscc_jit_stop(void);

#ifdef __cplusplus
}
#endif
#endif
