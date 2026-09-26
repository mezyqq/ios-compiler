// ioscc — clang-драйвер и lld::macho внутри процесса.
// cc1 выполняется в том же процессе (как clang с -fintegrated-cc1): повторяет cc1_main из
// clang/tools/driver, которого нет в библиотеках. Падения cc1 и lld ловит CrashRecoveryContext.
#include "ioscc.h"

#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticOptions.h"
#include "clang/CodeGen/ObjectFilePCHContainerWriter.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/TextDiagnosticBuffer.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/FrontendTool/Utils.h"
#include "clang/Serialization/ObjectFilePCHContainerReader.h"
#include "clang/Driver/CreateInvocationFromArgs.h"
#include "clang/Format/Format.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "clang/Sema/CodeCompleteConsumer.h"
#include "clang/Sema/Sema.h"
#include "clang/Tooling/Core/Replacement.h"
#include "lld/Common/Driver.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/ExecutionEngine/JITLink/JITLink.h"
#include "llvm/ExecutionEngine/Orc/AbsoluteSymbols.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/ObjectFileInterface.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <mutex>
#include <objc/runtime.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <unistd.h>

LLD_HAS_DRIVER(macho)

using namespace clang;

static void initTargets() {
	static std::once_flag once;
	std::call_once(once, [] {
		llvm::InitializeAllTargets();
		llvm::InitializeAllTargetMCs();
		llvm::InitializeAllAsmPrinters();
		llvm::InitializeAllAsmParsers();
	});
}

// report_fatal_error: печатаем через диагностику; дальше LLVM вызывает Process::Exit,
// а он внутри CrashRecoveryContext возвращает управление вместо exit().
static void fatalHandler(void *ud, const char *msg, bool) {
	auto &diags = *static_cast<DiagnosticsEngine *>(ud);
	diags.Report(diags.getCustomDiagID(DiagnosticsEngine::Error, "%0")) << msg;
}

static int cc1(llvm::SmallVectorImpl<const char *> &argv) {
	// опции LLVM глобальные, и драйвер мог их уже тронуть
	llvm::cl::ResetAllOptionOccurrences();
	if (argv.size() < 2 || llvm::StringRef(argv[1]) != "-cc1") {
		llvm::errs() << "ioscc: поддерживается только -cc1 (ассемблер .s не встроен)\n";
		return 1;
	}
	llvm::ArrayRef<const char *> args = llvm::ArrayRef(argv).slice(1);

	auto pch = std::make_shared<PCHContainerOperations>();
	pch->registerWriter(std::make_unique<ObjectFilePCHContainerWriter>());
	pch->registerReader(std::make_unique<ObjectFilePCHContainerReader>());

	DiagnosticOptions bufOpts;
	auto *buf = new TextDiagnosticBuffer;
	DiagnosticsEngine bufDiags(DiagnosticIDs::create(), bufOpts, buf);
	auto inv = std::make_shared<CompilerInvocation>();
	bool ok = CompilerInvocation::CreateFromArgs(*inv, args, bufDiags, argv[0]);

	auto ci = std::make_unique<CompilerInstance>(std::move(inv), std::move(pch));
	// драйвер передаёт -disable-free (утечка ради скорости у одноразового процесса) — нам нужно освобождать
	ci->getFrontendOpts().DisableFree = false;
	ci->createVirtualFileSystem(llvm::vfs::getRealFileSystem(), buf);
	ci->createDiagnostics();
	llvm::install_fatal_error_handler(fatalHandler, &ci->getDiagnostics());
	buf->FlushDiagnostics(ci->getDiagnostics());
	if (ok) ok = ExecuteCompilerInvocation(ci.get());
	else ci->getDiagnosticClient().finish();
	llvm::remove_fatal_error_handler();
	llvm::errs().flush();
	return ok ? 0 : 1;
}

extern "C" int ioscc_cc(int argc, const char **argv) {
	initTargets();
	llvm::SmallVector<const char *, 64> args(argv, argv + argc);

	DiagnosticOptions opts;
	auto *printer = new TextDiagnosticPrinter(llvm::errs(), opts);
	printer->setPrefix("clang");
	DiagnosticsEngine diags(DiagnosticIDs::create(), opts, printer);

	driver::Driver drv(argv[0], "arm64-apple-ios", diags, "clang LLVM compiler",
	                   llvm::vfs::getRealFileSystem());
	auto run = [](llvm::SmallVectorImpl<const char *> &a) { return cc1(a); };
	drv.CC1Main = run;
	// CC1Command запускает cc1 внутри CrashRecoveryContext: падение компилятора — ошибка, а не вылет IDE
	llvm::CrashRecoveryContext::Enable();

	std::unique_ptr<driver::Compilation> c(drv.BuildCompilation(args));
	int res = 1;
	if (c && !c->containsError()) {
		llvm::SmallVector<std::pair<int, const driver::Command *>, 4> failing;
		res = drv.ExecuteCompilation(*c, failing);
		for (auto &f : failing)
			if (!res) res = f.first;
	}
	llvm::CrashRecoveryContext::Disable();
	diags.getClient()->finish();
	llvm::errs().flush();
	return res;
}

extern "C" int ioscc_ld(int argc, const char **argv) {
	static bool broken = false;
	if (broken) {
		llvm::errs() << "ld: линковщик упал в прошлый раз — перезапусти приложение\n";
		return 125;
	}
	initTargets(); // для LTO
	std::vector<const char *> args{"ld64.lld"};
	args.insert(args.end(), argv + (argc > 0), argv + argc);
	lld::Result r = lld::lldMain(args, llvm::outs(), llvm::errs(), {{lld::Darwin, &lld::macho::link}});
	llvm::outs().flush();
	llvm::errs().flush();
	if (!r.canRunAgain) broken = true;
	return r.retCode;
}

extern "C" const char *ioscc_version(void) { return LLVM_VERSION_STRING; }

// MARK: редактор — проверка, автодополнение, формат

// Разбор path (содержимое — text) с -fsyntax-only; setup донастраивает вызов (автодополнение).
static void parseInMemory(int argc, const char **argv, const char *path, const char *text,
                          DiagnosticConsumer *diag, llvm::function_ref<void(CompilerInvocation &)> setup,
                          CodeCompleteConsumer *complete) {
	initTargets();
	llvm::cl::ResetAllOptionOccurrences();
	std::vector<const char *> args(argv, argv + argc);
	args.push_back("-fsyntax-only");
	args.push_back(path);

	DiagnosticOptions dopts;
	auto diags = llvm::makeIntrusiveRefCnt<DiagnosticsEngine>(DiagnosticIDs::create(), dopts, diag, false);
	CreateInvocationOptions o;
	o.Diags = diags;
	o.VFS = llvm::vfs::getRealFileSystem();
	o.RecoverOnError = true;
	std::shared_ptr<CompilerInvocation> inv = createInvocation(args, o);
	if (!inv) return;
	inv->getPreprocessorOpts().addRemappedFile(path, llvm::MemoryBuffer::getMemBufferCopy(text, path).release());
	inv->getFrontendOpts().DisableFree = false;
	setup(*inv);

	auto pch = std::make_shared<PCHContainerOperations>();
	pch->registerReader(std::make_unique<ObjectFilePCHContainerReader>());
	pch->registerWriter(std::make_unique<ObjectFilePCHContainerWriter>());
	CompilerInstance ci(std::move(inv), std::move(pch));
	ci.createVirtualFileSystem(llvm::vfs::getRealFileSystem(), diag);
	ci.createDiagnostics(diag, false);
	if (complete) ci.setCodeCompletionConsumer(complete);
	llvm::install_fatal_error_handler(fatalHandler, &ci.getDiagnostics());
	llvm::CrashRecoveryContext crc;
	crc.RunSafely([&] {
		SyntaxOnlyAction act;
		ci.ExecuteAction(act);
	});
	llvm::remove_fatal_error_handler();
}

extern "C" char *ioscc_check(int argc, const char **argv, const char *path, const char *text) {
	std::string out;
	llvm::raw_string_ostream os(out);
	DiagnosticOptions opts;
	opts.ShowCarets = false;
	opts.ShowColors = false;
	TextDiagnosticPrinter printer(os, opts);
	llvm::CrashRecoveryContext::Enable();
	parseInMemory(argc, argv, path, text, &printer, [](CompilerInvocation &) {}, nullptr);
	llvm::CrashRecoveryContext::Disable();
	os.flush();
	return strdup(out.c_str());
}

namespace {
// Собирает варианты в строки «вид\tимя\tвставка\tподпись\tтип».
class Collect : public CodeCompleteConsumer {
	CodeCompletionTUInfo info{std::make_shared<GlobalCodeCompletionAllocator>()};
	std::string &out;
	int max;

public:
	Collect(const CodeCompleteOptions &o, std::string &out, int max) : CodeCompleteConsumer(o), out(out), max(max) {}
	CodeCompletionAllocator &getAllocator() override { return info.getAllocator(); }
	CodeCompletionTUInfo &getCodeCompletionTUInfo() override { return info; }

	void ProcessCodeCompleteResults(Sema &S, CodeCompletionContext ctx, CodeCompletionResult *R, unsigned n) override {
		llvm::StringRef filter = S.getPreprocessor().getCodeCompletionFilter();
		std::vector<std::pair<unsigned, std::string>> items;
		for (unsigned i = 0; i < n; i++) {
			CodeCompletionResult &r = R[i];
			if (r.Availability == CXAvailability_NotAvailable || r.Hidden) continue;
			CodeCompletionString *cs = r.CreateCodeCompletionString(S, ctx, getAllocator(), info, false);
			if (!cs) continue;
			const char *typed = cs->getTypedText();
			if (!typed || !*typed || (typed[0] == '_' && (filter.empty() || filter[0] != '_'))) continue;
			if (!filter.empty() && !llvm::StringRef(typed).starts_with_insensitive(filter)) continue;
			std::string insert, label, result;
			for (const auto &ch : *cs) {
				switch (ch.Kind) {
				case CodeCompletionString::CK_ResultType: result = ch.Text; break;
				case CodeCompletionString::CK_Optional: break;
				case CodeCompletionString::CK_Informative: label += ch.Text; break;
				case CodeCompletionString::CK_Placeholder:
					label += ch.Text;
					insert += std::string("<#") + ch.Text + "#>";
					break;
				case CodeCompletionString::CK_VerticalSpace: label += ' '; insert += ' '; break;
				default:
					if (ch.Text) { label += ch.Text; insert += ch.Text; }
				}
			}
			const char *kind = "pattern";
			if (r.Kind == CodeCompletionResult::RK_Keyword) kind = "keyword";
			else if (r.Kind == CodeCompletionResult::RK_Macro) kind = "macro";
			else if (r.Declaration) kind = r.Declaration->getDeclKindName();
			for (auto *s : {&insert, &label, &result})
				for (char &c : *s)
					if (c == '\t' || c == '\n') c = ' ';
			items.push_back({cs->getPriority(), std::string(kind) + "\t" + typed + "\t" + insert + "\t" + label + "\t" + result});
		}
		std::stable_sort(items.begin(), items.end(), [](auto &a, auto &b) { return a.first < b.first; });
		for (size_t i = 0; i < items.size() && (int)i < max; i++) out += items[i].second + "\n";
	}
};
} // namespace

extern "C" char *ioscc_complete(int argc, const char **argv, const char *path, const char *text, int line, int col, int max) {
	std::string out;
	IgnoringDiagConsumer quiet;
	CodeCompleteOptions o;
	o.IncludeMacros = true;
	o.IncludeGlobals = true;
	o.IncludeCodePatterns = true;
	o.IncludeBriefComments = false;
	auto *c = new Collect(o, out, max);  // CompilerInstance забирает владение
	llvm::CrashRecoveryContext::Enable();
	parseInMemory(argc, argv, path, text, &quiet, [&](CompilerInvocation &inv) {
		inv.getFrontendOpts().CodeCompletionAt = ParsedSourceLocation{path, (unsigned)line, (unsigned)col};
		inv.getFrontendOpts().CodeCompleteOpts = o;
		inv.getDiagnosticOpts().IgnoreWarnings = true;
	}, c);
	llvm::CrashRecoveryContext::Disable();
	return strdup(out.c_str());
}

extern "C" char *ioscc_format(const char *code, const char *path, const char *style) {
	using namespace clang::format;
	llvm::StringRef src(code);
	FormatStyle st = getLLVMStyle(guessLanguage(path, src));
	if (style && *style) {
		if (std::error_code ec = parseConfiguration(style, &st)) {
			llvm::errs() << ".clang-format: " << ec.message() << "\n";
			return nullptr;
		}
	} else {
		st.UseTab = FormatStyle::UT_ForIndentation;
		st.IndentWidth = 4;
		st.TabWidth = 4;
		st.ObjCBlockIndentWidth = 4;
		st.ContinuationIndentWidth = 4;
		st.ColumnLimit = 0;
	}
	tooling::Replacements repl = reformat(st, src, {tooling::Range(0, src.size())}, path);
	llvm::Expected<std::string> res = tooling::applyAllReplacements(src, repl);
	if (!res) {
		llvm::errs() << "clang-format: " << llvm::toString(res.takeError()) << "\n";
		return nullptr;
	}
	return strdup(res->c_str());
}

// MARK: JIT

extern "C" int csops(pid_t pid, unsigned ops, void *useraddr, size_t usersize);
// objc4: регистрирует класс из загруженного вручную образа (так делает и lldb)
extern "C" Class objc_readClassPair(Class cls, const void *imageInfo);

struct ioscc_jit {
	std::unique_ptr<llvm::orc::LLJIT> J;
	// разделы, найденные при линковке: ObjC и конструкторы
	std::vector<std::pair<uint64_t, uint64_t>> selrefs, classlist, inits;
	uint64_t imageInfo = 0;
	std::vector<void (*)(void)> atexits;
};

namespace {
ioscc_jit *linking;  // сессия, которую сейчас линкуем (плагин общий для слоя)

bool keepSection(llvm::StringRef n) {
	return n.ends_with("__objc_selrefs") || n.ends_with("__objc_classlist") || n.ends_with("__objc_imageinfo") ||
	       n.ends_with("__mod_init_func") || n.ends_with("__objc_catlist") || n.ends_with("__objc_protolist");
}

// Без платформы ORC (нужен orc_rt) эти разделы никто не держит — JITLink их выбросит. Держим и запоминаем адреса.
class ObjCPlugin : public llvm::orc::ObjectLinkingLayer::Plugin {
public:
	void modifyPassConfig(llvm::orc::MaterializationResponsibility &, llvm::jitlink::LinkGraph &,
	                      llvm::jitlink::PassConfiguration &C) override {
		C.PrePrunePasses.push_back([](llvm::jitlink::LinkGraph &G) {
			for (auto &S : G.sections())
				if (keepSection(S.getName()))
					for (auto *B : S.blocks()) G.addAnonymousSymbol(*B, 0, B->getSize(), false, true);
			return llvm::Error::success();
		});
		C.PostFixupPasses.push_back([](llvm::jitlink::LinkGraph &G) {
			if (!linking) return llvm::Error::success();
			for (auto &S : G.sections()) {
				llvm::jitlink::SectionRange r(S);
				if (r.empty()) continue;
				std::pair<uint64_t, uint64_t> rg{r.getStart().getValue(), r.getEnd().getValue()};
				llvm::StringRef n = S.getName();
				if (n.ends_with("__objc_selrefs")) linking->selrefs.push_back(rg);
				else if (n.ends_with("__objc_classlist")) linking->classlist.push_back(rg);
				else if (n.ends_with("__mod_init_func")) linking->inits.push_back(rg);
				else if (n.ends_with("__objc_imageinfo")) linking->imageInfo = rg.first;
			}
			return llvm::Error::success();
		});
	}
	llvm::Error notifyFailed(llvm::orc::MaterializationResponsibility &) override { return llvm::Error::success(); }
	llvm::Error notifyRemovingResources(llvm::orc::JITDylib &, llvm::orc::ResourceKey) override { return llvm::Error::success(); }
	void notifyTransferringResources(llvm::orc::JITDylib &, llvm::orc::ResourceKey, llvm::orc::ResourceKey) override {}
};

// exit()/abort() из JIT-кода не должны закрывать Forge: прыгаем обратно в ioscc_jit_main
sigjmp_buf jumpBack;
volatile sig_atomic_t guarded = 0;
pthread_t guardThread;
ioscc_jit *running;
struct sigaction oldActions[NSIG];
const int caught[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP, SIGABRT, SIGUSR2};

void jitExit(int code) {
	fflush(stdout);
	siglongjmp(jumpBack, 0x100 | (code & 0xff));
}
void jitAbort() { siglongjmp(jumpBack, 128 + SIGABRT); }
int jitAtexit(void (*fn)(void)) {
	if (running) running->atexits.push_back(fn);
	return 0;
}

void onSignal(int sig, siginfo_t *, void *) {
	if (guarded && pthread_equal(pthread_self(), guardThread)) siglongjmp(jumpBack, 128 + sig);
	// не наш поток — вернуть прежний обработчик (журнал вылетов Forge) и дать сигналу повториться
	sigaction(sig, &oldActions[sig], nullptr);
	if (sig == SIGUSR2 || sig == SIGABRT) raise(sig);
}

// Запуск fn под защитой: возвращает 0 или код (exit — код, падение — 128+сигнал).
template <typename F> int guardedCall(F fn) {
	guardThread = pthread_self();
	struct sigaction sa = {};
	sa.sa_sigaction = onSignal;
	sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
	for (int s : caught) sigaction(s, &sa, &oldActions[s]);
	int r = sigsetjmp(jumpBack, 1);
	if (r == 0) {
		guarded = 1;
		fn();
	}
	guarded = 0;
	for (int s : caught) sigaction(s, &oldActions[s], nullptr);
	fflush(stdout);
	if (r >= 0x100) return r & 0xff;
	if (r >= 128) {
		const char *what = r == 128 + SIGUSR2 ? "остановлено" : strsignal(r - 128);
		fprintf(stderr, "\n%s\n", what);
	}
	return r;
}

void registerObjC(ioscc_jit *s) {
	static const uint32_t defaultInfo[2] = {0, 0x40};
	for (auto &rg : s->selrefs)
		for (uint64_t a = rg.first; a + 8 <= rg.second; a += 8) {
			auto **p = reinterpret_cast<const char **>(a);
			*p = sel_getName(sel_registerName(*p));
		}
	const void *info = s->imageInfo ? reinterpret_cast<const void *>(s->imageInfo) : defaultInfo;
	std::vector<Class> pending;
	for (auto &rg : s->classlist)
		for (uint64_t a = rg.first; a + 8 <= rg.second; a += 8) pending.push_back(*reinterpret_cast<Class *>(a));
	// суперкласс должен быть зарегистрирован раньше подкласса — несколько проходов
	while (!pending.empty()) {
		std::vector<Class> next;
		for (Class c : pending) {
			Class super = class_getSuperclass(c);
			bool superPending = std::find(pending.begin(), pending.end(), super) != pending.end() && super != c;
			if (superPending) next.push_back(c);
			else objc_readClassPair(c, info);
		}
		if (next.size() == pending.size()) {
			for (Class c : next) objc_readClassPair(c, info);
			break;
		}
		pending.swap(next);
	}
}
} // namespace

extern "C" int ioscc_jit_enabled(void) {
	uint32_t flags = 0;
	if (csops(getpid(), 0 /* CS_OPS_STATUS */, &flags, sizeof flags) != 0) return 0;
	return (flags & 0x10000000 /* CS_DEBUGGED */) != 0;
}

extern "C" ioscc_jit *ioscc_jit_load(int n, const char **objects) {
	using namespace llvm::orc;
	initTargets();
	auto fail = [](llvm::Error e) -> ioscc_jit * {
		llvm::errs() << "jit: " << llvm::toString(std::move(e)) << "\n";
		llvm::errs().flush();
		return nullptr;
	};
	auto J = LLJITBuilder()
	             .setObjectLinkingLayerCreator([](ExecutionSession &ES) -> llvm::Expected<std::unique_ptr<ObjectLayer>> {
		             auto L = std::make_unique<ObjectLinkingLayer>(ES);
		             L->addPlugin(std::make_shared<ObjCPlugin>());
		             return L;
	             })
	             .create();
	if (!J) return fail(J.takeError());

	auto *s = new ioscc_jit;
	s->J = std::move(*J);
	auto &JD = s->J->getMainJITDylib();
	SymbolMap hooks;
	auto flags = llvm::JITSymbolFlags::Exported | llvm::JITSymbolFlags::Callable;
	hooks[s->J->mangleAndIntern("exit")] = {ExecutorAddr::fromPtr(&jitExit), flags};
	hooks[s->J->mangleAndIntern("_exit")] = {ExecutorAddr::fromPtr(&jitExit), flags};
	hooks[s->J->mangleAndIntern("abort")] = {ExecutorAddr::fromPtr(&jitAbort), flags};
	hooks[s->J->mangleAndIntern("atexit")] = {ExecutorAddr::fromPtr(&jitAtexit), flags};
	if (auto e = JD.define(absoluteSymbols(std::move(hooks)))) return fail(std::move(e));

	// все символы всех файлов — чтобы слинковались и файлы, на которые никто не ссылается (ObjC-классы)
	SymbolLookupSet all;
	for (int i = 0; i < n; i++) {
		auto buf = llvm::MemoryBuffer::getFile(objects[i]);
		if (!buf) return fail(llvm::createStringError(buf.getError(), "%s", objects[i]));
		auto iface = getObjectFileInterface(s->J->getExecutionSession(), (*buf)->getMemBufferRef());
		if (!iface) return fail(iface.takeError());
		for (auto &kv : iface->SymbolFlags) all.add(kv.first, SymbolLookupFlags::WeaklyReferencedSymbol);
		if (auto e = s->J->addObjectFile(std::move(*buf))) return fail(std::move(e));
	}
	linking = s;
	auto r = s->J->getExecutionSession().lookup(makeJITDylibSearchOrder(&JD), std::move(all));
	linking = nullptr;
	if (!r) return fail(r.takeError());

	registerObjC(s);
	running = s;
	int code = guardedCall([s] {
		for (auto &rg : s->inits)
			for (uint64_t a = rg.first; a + 8 <= rg.second; a += 8) (*reinterpret_cast<void (**)(void)>(a))();
	});
	running = nullptr;
	if (code) {
		fprintf(stderr, "jit: статический конструктор завершился с кодом %d\n", code);
		return nullptr;
	}
	return s;
}

extern "C" void *ioscc_jit_lookup(ioscc_jit *s, const char *name) {
	auto sym = s->J->lookup(name);
	if (!sym) {
		llvm::consumeError(sym.takeError());
		return nullptr;
	}
	return sym->toPtr<void *>();
}

extern "C" int ioscc_jit_main(ioscc_jit *s, int argc, const char **argv) {
	auto main = reinterpret_cast<int (*)(int, const char **)>(ioscc_jit_lookup(s, "main"));
	if (!main) {
		fprintf(stderr, "jit: нет функции main\n");
		return 1;
	}
	running = s;
	s->atexits.clear();
	int ret = 0;
	int code = guardedCall([&] { ret = main(argc, argv); });
	if (code == 0) code = ret;
	// atexit-обработчики — в обратном порядке, как у exit()
	auto handlers = s->atexits;
	for (auto it = handlers.rbegin(); it != handlers.rend(); ++it) guardedCall(*it);
	running = nullptr;
	return code;
}

extern "C" void *ioscc_jit_call(ioscc_jit *s, const char *name, int *status) {
	auto fn = reinterpret_cast<void *(*)(void)>(ioscc_jit_lookup(s, name));
	if (!fn) {
		*status = -1;
		return nullptr;
	}
	void *res = nullptr;
	*status = guardedCall([&] { res = fn(); });
	return res;
}

extern "C" void ioscc_jit_stop(void) {
	if (guarded) pthread_kill(guardThread, SIGUSR2);
}
