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
#include "lld/Common/Driver.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <mutex>

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
