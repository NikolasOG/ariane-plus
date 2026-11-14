// SA-MP edition: crash logger.
//
// On an unhandled exception, writes next to ariane.exe:
//   crash-YYYYMMDD-HHMMSS.txt  readable stack (function + file:line via ariane.pdb)
//   crash-YYYYMMDD-HHMMSS.dmp  minidump for a debugger
#ifdef _WIN32
#include <windows.h>
#include <dbghelp.h>
#include <stdio.h>
#include <time.h>

#pragma comment(lib, "dbghelp.lib")

static char gCrashDir[MAX_PATH];

static void
crashPath(char *dst, size_t size, const char *stamp, const char *ext)
{
	snprintf(dst, size, "%s\\crash-%s.%s", gCrashDir, stamp, ext);
}

static LONG WINAPI
CrashFilter(EXCEPTION_POINTERS *info)
{
	char stamp[32], path[MAX_PATH];
	time_t now = time(nullptr);
	struct tm local;
	localtime_s(&local, &now);
	strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);

	// minidump first: it's the most valuable artefact if anything below fails
	crashPath(path, sizeof(path), stamp, "dmp");
	HANDLE dump = CreateFileA(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if(dump != INVALID_HANDLE_VALUE){
		MINIDUMP_EXCEPTION_INFORMATION mei = { GetCurrentThreadId(), info, FALSE };
		MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump,
			(MINIDUMP_TYPE)(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory),
			&mei, nullptr, nullptr);
		CloseHandle(dump);
	}

	crashPath(path, sizeof(path), stamp, "txt");
	FILE *f = fopen(path, "w");
	if(f == nullptr)
		return EXCEPTION_CONTINUE_SEARCH;
	EXCEPTION_RECORD *rec = info->ExceptionRecord;
	fprintf(f, "Ariane (SA-MP edition) crash %s\n", stamp);
	fprintf(f, "exception 0x%08lX at %p\n", rec->ExceptionCode, rec->ExceptionAddress);
	if(rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2)
		fprintf(f, "access violation %s address %p\n",
			rec->ExceptionInformation[0] == 0 ? "reading" : rec->ExceptionInformation[0] == 1 ? "writing" : "executing",
			(void*)rec->ExceptionInformation[1]);
	fprintf(f, "\nstack:\n");

	HANDLE process = GetCurrentProcess(), thread = GetCurrentThread();
	SymSetOptions(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
	SymInitialize(process, gCrashDir, TRUE);

	CONTEXT ctx = *info->ContextRecord;
	STACKFRAME64 frame = {};
#ifdef _M_X64
	DWORD machine = IMAGE_FILE_MACHINE_AMD64;
	frame.AddrPC.Offset = ctx.Rip;
	frame.AddrFrame.Offset = ctx.Rbp;
	frame.AddrStack.Offset = ctx.Rsp;
#else
	DWORD machine = IMAGE_FILE_MACHINE_I386;
	frame.AddrPC.Offset = ctx.Eip;
	frame.AddrFrame.Offset = ctx.Ebp;
	frame.AddrStack.Offset = ctx.Esp;
#endif
	frame.AddrPC.Mode = frame.AddrFrame.Mode = frame.AddrStack.Mode = AddrModeFlat;

	char symBuf[sizeof(SYMBOL_INFO) + 256];
	for(int depth = 0; depth < 48; depth++){
		if(!StackWalk64(machine, process, thread, &frame, &ctx, nullptr,
		                SymFunctionTableAccess64, SymGetModuleBase64, nullptr) || frame.AddrPC.Offset == 0)
			break;
		DWORD64 addr = frame.AddrPC.Offset;
		char module[MAX_PATH] = "?";
		HMODULE hmod = nullptr;
		if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		                      (LPCSTR)addr, &hmod)){
			GetModuleFileNameA(hmod, module, sizeof(module));
			char *slash = strrchr(module, '\\');
			if(slash) memmove(module, slash + 1, strlen(slash));
		}
		SYMBOL_INFO *sym = (SYMBOL_INFO*)symBuf;
		memset(symBuf, 0, sizeof(symBuf));
		sym->SizeOfStruct = sizeof(SYMBOL_INFO);
		sym->MaxNameLen = 255;
		DWORD64 symDisp = 0;
		IMAGEHLP_LINE64 line = { sizeof(IMAGEHLP_LINE64) };
		DWORD lineDisp = 0;
		bool haveSym = SymFromAddr(process, addr, &symDisp, sym) != FALSE;
		bool haveLine = SymGetLineFromAddr64(process, addr, &lineDisp, &line) != FALSE;
		fprintf(f, "#%-2d %-18s %s+0x%llx", depth, module, haveSym ? sym->Name : "?", (unsigned long long)symDisp);
		if(haveLine)
			fprintf(f, "  (%s:%lu)", line.FileName, line.LineNumber);
		fprintf(f, "\n");
	}
	SymCleanup(process);
	fclose(f);
	return EXCEPTION_CONTINUE_SEARCH;
}

void
InstallCrashLogger(void)
{
	DWORD n = GetModuleFileNameA(nullptr, gCrashDir, sizeof(gCrashDir));
	if(n == 0 || n >= sizeof(gCrashDir))
		return;
	char *slash = strrchr(gCrashDir, '\\');
	if(slash) *slash = '\0';
	SetUnhandledExceptionFilter(CrashFilter);

	// ARIANE_CRASH_TEST=1 crashes on purpose to check the logger end to end.
	char flag[8];
	if(GetEnvironmentVariableA("ARIANE_CRASH_TEST", flag, sizeof(flag)) > 0 && flag[0] == '1'){
		volatile int *nothing = nullptr;
		*nothing = 1;
	}
}
#else
void InstallCrashLogger(void) {}
#endif
