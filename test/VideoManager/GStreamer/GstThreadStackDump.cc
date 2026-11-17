#include "GstThreadStackDump.h"

#include <QtCore/QDir>
#include <QtCore/QFile>

#ifdef Q_OS_WIN
#include <array>
#include <atomic>
#include <bit>
#include <memory>
#include <mutex>
#include <process.h>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>
#include <windows.h>

// These SDK headers require the Windows types above, including outside a precompiled header build.
#include <dbghelp.h>
#include <tlhelp32.h>

#include <QtCore/QScopeGuard>
#endif

namespace GstTest {

QString dumpAllThreadStacks()
{
    QString dump = QStringLiteral("=== h265 failure thread dump ===\n");
#ifdef Q_OS_WIN
#if defined(_M_X64) || defined(__x86_64__) || defined(_M_ARM64) || defined(__aarch64__)
    using WinHandle = std::unique_ptr<void, decltype(&CloseHandle)>;

    struct ThreadRecord
    {
        DWORD id;
        WinHandle handle;
        DWORD openError;
        QString description;
    };

    // Handles outlive the watchdog, including its timeout recovery of the suspended thread.
    std::vector<ThreadRecord> threads;
    const WinHandle finished(CreateEventW(nullptr, TRUE, FALSE, nullptr), &CloseHandle);
    const WinHandle ready(CreateEventW(nullptr, TRUE, FALSE, nullptr), &CloseHandle);
    if (!finished || !ready) {
        return dump + QStringLiteral("Watchdog events unavailable: error=%1\n").arg(GetLastError());
    }
    std::atomic<DWORD> watchdogId{0};
    std::atomic<HANDLE> suspendedThread{nullptr};
    const HANDLE stderrHandle = GetStdHandle(STD_ERROR_HANDLE);
    std::thread watchdog;
    try {
        watchdog = std::thread([&]() {
            watchdogId.store(GetCurrentThreadId());
            SetEvent(ready.get());
            if (WaitForSingleObject(finished.get(), 90000) == WAIT_OBJECT_0) {
                return;
            }
            if (const HANDLE thread = suspendedThread.load()) {
                ResumeThread(thread);
            }
            // Bypass CRT/Qt stderr locks: one of their owners may have been suspended.
            constexpr char message[] = "h265 thread dump watchdog: exceeded 90 s; resume attempted; exiting\n";
            DWORD written = 0;
            WriteFile(stderrHandle, message, static_cast<DWORD>(sizeof(message) - 1), &written, nullptr);
            _exit(124);
        });
    } catch (const std::system_error&) {
        return dump + QStringLiteral("Watchdog thread unavailable; refusing to suspend threads\n");
    }
    const auto stopWatchdog = qScopeGuard([&]() {
        if (const HANDLE thread = suspendedThread.exchange(nullptr)) {
            ResumeThread(thread);
        }
        SetEvent(finished.get());
        watchdog.join();
    });
    WaitForSingleObject(ready.get(), INFINITE);

    // DbgHelp is process-global and single-threaded. Keep this test helper's calls serialized.
    static std::mutex symbolMutex;
    const std::lock_guard symbolLock(symbolMutex);
    const HANDLE process = GetCurrentProcess();
    static const DWORD symbolError = []() {
        // Deferred loading: invading the process must not read every module's PDB inside the watchdog window.
        SymSetOptions(SymGetOptions() | SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_FAIL_CRITICAL_ERRORS |
                      SYMOPT_NO_PROMPTS);
        return SymInitialize(GetCurrentProcess(), nullptr, TRUE) ? ERROR_SUCCESS : GetLastError();
    }();
    if (symbolError != ERROR_SUCCESS) {
        return dump + QStringLiteral("SymInitialize failed: error=%1\n").arg(symbolError);
    }

    using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    const HMODULE kernelBase = GetModuleHandleW(L"KernelBase.dll");
    const auto getDescription =
        kernelBase ? std::bit_cast<GetDescription>(GetProcAddress(kernelBase, "GetThreadDescription")) : nullptr;
    const HANDLE rawSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (rawSnapshot == INVALID_HANDLE_VALUE) {
        return dump + QStringLiteral("Thread snapshot failed: error=%1\n").arg(GetLastError());
    }
    const WinHandle snapshot(rawSnapshot, &CloseHandle);
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    const BOOL haveThread = Thread32First(snapshot.get(), &entry);
    if (!haveThread) {
        return dump + QStringLiteral("Thread enumeration failed: error=%1\n").arg(GetLastError());
    }
    const DWORD callerId = GetCurrentThreadId();
    do {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == callerId ||
            entry.th32ThreadID == watchdogId.load()) {
            continue;
        }
        WinHandle handle(OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                    entry.th32ThreadID),
                         &CloseHandle);
        const DWORD openError = handle ? ERROR_SUCCESS : GetLastError();
        QString description;
        PWSTR name = nullptr;
        if (handle && getDescription && SUCCEEDED(getDescription(handle.get(), &name)) && name) {
            description = QString::fromWCharArray(name);
        }
        if (name) {
            LocalFree(name);
        }
        threads.push_back({entry.th32ThreadID, std::move(handle), openError, std::move(description)});
    } while (Thread32Next(snapshot.get(), &entry));
    if (const DWORD error = GetLastError(); error != ERROR_NO_MORE_FILES) {
        dump += QStringLiteral("Thread enumeration incomplete: error=%1\n").arg(error);
    }

    dump += QStringLiteral("Windows pid=%1 caller=%2 (caller/watchdog excluded); max 40 frames per thread\n")
                .arg(GetCurrentProcessId())
                .arg(callerId);
    for (const ThreadRecord& thread : threads) {
        dump +=
            QStringLiteral("tid=%1 (0x%2) name=\"%3\"\n").arg(thread.id).arg(thread.id, 0, 16).arg(thread.description);
        if (!thread.handle) {
            dump += QStringLiteral("  OpenThread failed: error=%1\n").arg(thread.openError);
            continue;
        }
        std::array<DWORD64, 40> addresses{};
        size_t frameCount = 0;
        CONTEXT context{};
        context.ContextFlags = CONTEXT_FULL;
        STACKFRAME64 frame{};
        frame.AddrPC.Mode = AddrModeFlat;
        frame.AddrFrame.Mode = AddrModeFlat;
        frame.AddrStack.Mode = AddrModeFlat;
        suspendedThread.store(thread.handle.get());
        if (SuspendThread(thread.handle.get()) == static_cast<DWORD>(-1)) {
            const DWORD error = GetLastError();
            suspendedThread.store(nullptr);
            dump += QStringLiteral("  SuspendThread failed: error=%1\n").arg(error);
            continue;
        }
        const BOOL haveContext = GetThreadContext(thread.handle.get(), &context);
        const DWORD contextError = haveContext ? ERROR_SUCCESS : GetLastError();
        if (haveContext) {
#if defined(_M_ARM64) || defined(__aarch64__)
            constexpr DWORD MACHINE_TYPE = IMAGE_FILE_MACHINE_ARM64;
            frame.AddrPC.Offset = context.Pc;
            frame.AddrFrame.Offset = context.Fp;
            frame.AddrStack.Offset = context.Sp;
#else
            constexpr DWORD MACHINE_TYPE = IMAGE_FILE_MACHINE_AMD64;
            frame.AddrPC.Offset = context.Rip;
            frame.AddrFrame.Offset = context.Rbp;
            frame.AddrStack.Offset = context.Rsp;
#endif
            addresses[frameCount++] = frame.AddrPC.Offset;
            // Even StackWalk64 can allocate or take the loader lock. The watchdog bounds this risk.
            DWORD64 previousStack = frame.AddrStack.Offset;
            for (size_t step = 0; step < addresses.size() && frameCount < addresses.size(); ++step) {
                if (!StackWalk64(MACHINE_TYPE, process, thread.handle.get(), &frame, &context, nullptr,
                                 SymFunctionTableAccess64, SymGetModuleBase64, nullptr)) {
                    break;
                }
                if (!frame.AddrPC.Offset) {
                    break;
                }
                if (frame.AddrPC.Offset == addresses[frameCount - 1] && frame.AddrStack.Offset == previousStack) {
                    if (step == 0) {
                        continue;
                    }
                    break;
                }
                addresses[frameCount++] = frame.AddrPC.Offset;
                previousStack = frame.AddrStack.Offset;
            }
        }
        const DWORD resumed = ResumeThread(thread.handle.get());
        const DWORD resumeError = resumed == static_cast<DWORD>(-1) ? GetLastError() : ERROR_SUCCESS;
        if (resumeError == ERROR_SUCCESS) {
            suspendedThread.store(nullptr);
        }
        if (resumeError != ERROR_SUCCESS) {
            // Do not continue allocating while a thread may still be suspended; let the watchdog recover/exit.
            WaitForSingleObject(finished.get(), INFINITE);
        }
        if (!haveContext) {
            dump += QStringLiteral("  GetThreadContext failed: error=%1\n").arg(contextError);
        }
        // Resolve symbols and format only after resuming; the address array is filled without Qt/CRT allocation.
        for (size_t i = 0; i < frameCount; ++i) {
            alignas(SYMBOL_INFO) std::array<char, sizeof(SYMBOL_INFO) + MAX_SYM_NAME> symbolBuffer{};
            auto* const symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer.data());
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = MAX_SYM_NAME;
            DWORD64 displacement = 0;
            const bool haveSymbol = SymFromAddr(process, addresses[i], &displacement, symbol) != FALSE;
            IMAGEHLP_MODULE64 moduleInfo{};
            moduleInfo.SizeOfStruct = sizeof(moduleInfo);
            const bool haveModule = SymGetModuleInfo64(process, addresses[i], &moduleInfo) != FALSE;
            dump +=
                QStringLiteral("  #%1 0x%2 %3!%4+0x%5")
                    .arg(static_cast<qulonglong>(i))
                    .arg(addresses[i], 0, 16)
                    .arg(haveModule ? QString::fromLocal8Bit(moduleInfo.ModuleName) : QStringLiteral("?"),
                         haveSymbol ? QString::fromLocal8Bit(symbol->Name) : QStringLiteral("?"))
                    .arg(haveSymbol ? displacement : (haveModule ? addresses[i] - moduleInfo.BaseOfImage : 0), 0, 16);
            dump += u'\n';
        }
    }
#else
    dump += QStringLiteral("Windows stack walk unavailable for this architecture\n");
#endif
#elif defined(Q_OS_LINUX)
    const QDir tasks(QStringLiteral("/proc/self/task"));
    const QStringList tids = tasks.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    dump += QStringLiteral("Linux best effort: task states/wait channels, not native stacks\n");
    if (tids.isEmpty()) {
        dump += QStringLiteral("/proc/self/task unavailable\n");
    }
    for (const QString& tid : tids) {
        const auto readTask = [&tasks, &tid](const QString& fileName) {
            QFile file(tasks.filePath(tid + u'/' + fileName));
            return file.open(QIODevice::ReadOnly) ? QString::fromLocal8Bit(file.readAll()).trimmed()
                                                  : QStringLiteral("<unavailable>");
        };
        const QString stat = readTask(QStringLiteral("stat"));
        // comm in stat is parenthesized and can itself contain spaces or ')'. State follows the last ')'.
        const qsizetype stateOffset = stat.lastIndexOf(u')') + 2;
        const QString state =
            stateOffset > 1 && stateOffset < stat.size() ? stat.mid(stateOffset, 1) : QStringLiteral("?");
        dump += QStringLiteral("tid=%1 name=\"%2\" state=%3 wchan=%4\n")
                    .arg(tid, readTask(QStringLiteral("comm")), state, readTask(QStringLiteral("wchan")));
    }
#else
    return QStringLiteral("All-thread stacks unavailable on this platform\n");
#endif
    return dump + QStringLiteral("=== end h265 failure thread dump ===\n");
}

}  // namespace GstTest
