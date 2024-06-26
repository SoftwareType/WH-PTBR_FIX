#include "stdafx.h"

#include "all_processes_injector.h"
#include "critical_processes.h"
#include "dll_inject.h"
#include "functions.h"
#include "logger.h"
#include "session_private_namespace.h"
#include "storage_manager.h"

#ifndef STATUS_NO_MORE_ENTRIES
#define STATUS_NO_MORE_ENTRIES ((NTSTATUS)0x8000001AL)
#endif

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) ((NTSTATUS)(Status) >= 0)
#endif

namespace {

HANDLE CreateProcessInitAPCMutex(DWORD processId, BOOL initialOwner) {
    WCHAR szMutexName[SessionPrivateNamespace::kPrivateNamespaceMaxLen +
                      sizeof("\\ProcessInitAPCMutex-pid=1234567890")];
    int mutexNamePos =
        SessionPrivateNamespace::MakeName(szMutexName, GetCurrentProcessId());
    swprintf_s(szMutexName + mutexNamePos,
               ARRAYSIZE(szMutexName) - mutexNamePos,
               L"\\ProcessInitAPCMutex-pid=%u", processId);

    wil::unique_hlocal secDesc;
    THROW_IF_WIN32_BOOL_FALSE(
        Functions::GetFullAccessSecurityDescriptor(&secDesc, nullptr));

    SECURITY_ATTRIBUTES secAttr = {sizeof(SECURITY_ATTRIBUTES)};
    secAttr.lpSecurityDescriptor = secDesc.get();
    secAttr.bInheritHandle = FALSE;

    wil::unique_mutex_nothrow mutex(
        CreateMutex(&secAttr, initialOwner, szMutexName));
    THROW_LAST_ERROR_IF_NULL(mutex);

    return mutex.release();
}

HANDLE OpenProcessInitAPCMutex(DWORD processId, DWORD desiredAccess) {
    WCHAR szMutexName[SessionPrivateNamespace::kPrivateNamespaceMaxLen +
                      sizeof("\\ProcessInitAPCMutex-pid=1234567890")];
    int mutexNamePos =
        SessionPrivateNamespace::MakeName(szMutexName, GetCurrentProcessId());
    swprintf_s(szMutexName + mutexNamePos,
               ARRAYSIZE(szMutexName) - mutexNamePos,
               L"\\ProcessInitAPCMutex-pid=%u", processId);

    return OpenMutex(desiredAccess, FALSE, szMutexName);
}

}  // namespace

AllProcessesInjector::AllProcessesInjector() {
    HMODULE hNtdll = GetModuleHandle(L"ntdll.dll");
    THROW_LAST_ERROR_IF_NULL(hNtdll);

    m_NtGetNextProcess =
        (NtGetNextProcess_t)GetProcAddress(hNtdll, "NtGetNextProcess");
    THROW_LAST_ERROR_IF_NULL(m_NtGetNextProcess);

    m_NtGetNextThread =
        (NtGetNextThread_t)GetProcAddress(hNtdll, "NtGetNextThread");
    THROW_LAST_ERROR_IF_NULL(m_NtGetNextThread);

#ifdef _WIN64
    m_pRtlUserThreadStart =
        (DWORD64)GetProcAddress(hNtdll, "RtlUserThreadStart");
#else   // !_WIN64
    SYSTEM_INFO siSystemInfo;
    GetNativeSystemInfo(&siSystemInfo);
    if (siSystemInfo.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_INTEL) {
        // 32-bit machine.
        m_pRtlUserThreadStart =
            PTR_TO_DWORD64(GetProcAddress(hNtdll, "RtlUserThreadStart"));
    } else {
        DWORD64 hNtdll64 = GetModuleHandle64(L"ntdll.dll");
        if (hNtdll64 == 0) {
            THROW_WIN32(ERROR_MOD_NOT_FOUND);
        }

        m_pRtlUserThreadStart =
            GetProcAddress64(hNtdll64, "RtlUserThreadStart");
    }
#endif  // _WIN64
    THROW_LAST_ERROR_IF(m_pRtlUserThreadStart == 0);

    m_appPrivateNamespace =
        SessionPrivateNamespace::Create(GetCurrentProcessId());

    auto settings = StorageManager::GetInstance().GetAppConfig(L"Settings");
    m_includePattern = settings->GetString(L"Include").value_or(L"");
    m_excludePattern = settings->GetString(L"Exclude").value_or(L"");
    m_threadAttachExemptPattern =
        settings->GetString(L"ThreadAttachExempt").value_or(L"");

    if (!settings->GetInt(L"InjectIntoCriticalProcesses").value_or(0)) {
        if (!m_excludePattern.empty()) {
            m_excludePattern += L"|";
        }

        m_excludePattern += kCriticalProcesses;
    }
}

int AllProcessesInjector::InjectIntoNewProcesses() noexcept {
    int count = 0;

    while (true) {
        // Note: If we don't have the required permissions, the process is
        // skipped.
        HANDLE hNewProcess;
        NTSTATUS status = m_NtGetNextProcess(
            m_lastEnumeratedProcess.get(),
            SYNCHRONIZE | DllInject::kProcessAccess, 0, 0, &hNewProcess);
        if (!NT_SUCCESS(status)) {
            if (status != STATUS_NO_MORE_ENTRIES) {
                LOG(L"NtGetNextProcess error: %08X", status);
            }

            break;
        }

        m_lastEnumeratedProcess.reset(hNewProcess);

        if (WaitForSingleObject(hNewProcess, 0) == WAIT_OBJECT_0) {
            // Process is no longer alive.
            continue;
        }

        DWORD dwNewProcessId = GetProcessId(hNewProcess);
        if (dwNewProcessId == 0) {
            LOG(L"GetProcessId error: %u", GetLastError());
            continue;
        }

        try {
            bool threadAttachExempt;
            if (!ShouldSkipNewProcess(hNewProcess, dwNewProcessId,
                                      &threadAttachExempt)) {
                InjectIntoNewProcess(hNewProcess, dwNewProcessId,
                                     threadAttachExempt);
                count++;
            }
        } catch (const std::exception& e) {
            LOG(L"Error handling a new process %u: %S", dwNewProcessId,
                e.what());
        }
    }

    return count;
}

bool AllProcessesInjector::ShouldSkipNewProcess(HANDLE hProcess,
                                                DWORD dwProcessId,
                                                bool* threadAttachExempt) {
    auto processImageName =
        wil::QueryFullProcessImageName<std::wstring>(hProcess);

    if (Functions::DoesPathMatchPattern(processImageName, m_excludePattern) &&
        !Functions::DoesPathMatchPattern(processImageName, m_includePattern)) {
        VERBOSE(L"Skipping excluded process %u", dwProcessId);
        return true;
    }

    *threadAttachExempt = Functions::DoesPathMatchPattern(
        processImageName, m_threadAttachExemptPattern);

    return false;
}

void AllProcessesInjector::InjectIntoNewProcess(HANDLE hProcess,
                                                DWORD dwProcessId,
                                                bool threadAttachExempt) {
    // We check whether the process began running or not. If it didn't, it's
    // supposed to have only one thread which has its instruction pointer at
    // RtlUserThreadStart. For other cases, we assume the main thread was
    // resumed.
    //
    // If the process didn't begin running, creating a remote thread might be
    // too early and unsafe. One known problem with this is with console apps -
    // if we trigger console initialization (KERNELBASE!ConsoleCommitState)
    // before the parent process notified csrss.exe
    // (KERNELBASE!CsrClientCallServer), csrss.exe returns an access denied
    // error and the parent's CreateProcess call fails.
    //
    // If the process is the current process, we skip this check since it
    // obviously began running, and we don't want to suspend the current thread
    // and cause a deadlock.

    wil::unique_process_handle suspendedThread;

    if (dwProcessId != GetCurrentProcessId()) {
        DWORD processAccess = THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                              DllInject::kProcessAccess;

        wil::unique_process_handle thread1;
        THROW_IF_NTSTATUS_FAILED(m_NtGetNextThread(
            hProcess, nullptr, processAccess, 0, 0, &thread1));

        wil::unique_process_handle thread2;
        NTSTATUS status = m_NtGetNextThread(hProcess, thread1.get(),
                                            processAccess, 0, 0, &thread2);
        if (status == STATUS_NO_MORE_ENTRIES) {
            // Exactly one thread.
            DWORD previousSuspendCount = SuspendThread(thread1.get());
            THROW_LAST_ERROR_IF(previousSuspendCount == (DWORD)-1);

            if (previousSuspendCount == 0) {
                // The thread was already running.
                ResumeThread(thread1.get());
            } else {
                suspendedThread = std::move(thread1);
            }
        } else {
            THROW_IF_NTSTATUS_FAILED(status);
        }
    }

    if (suspendedThread) {
        auto suspendThreadCleanup = wil::scope_exit(
            [&suspendedThread] { ResumeThread(suspendedThread.get()); });

        bool threadNotStartedYet = false;

#ifdef _WIN64
        CONTEXT c;
        c.ContextFlags = CONTEXT_CONTROL;
        THROW_IF_WIN32_BOOL_FALSE(GetThreadContext(suspendedThread.get(), &c));
        if (c.Rip == m_pRtlUserThreadStart) {
            threadNotStartedYet = true;
        }
#else   // !_WIN64
        SYSTEM_INFO siSystemInfo;
        GetNativeSystemInfo(&siSystemInfo);
        if (siSystemInfo.wProcessorArchitecture ==
            PROCESSOR_ARCHITECTURE_INTEL) {
            // 32-bit machine.
            CONTEXT c;
            c.ContextFlags = CONTEXT_CONTROL;
            THROW_IF_WIN32_BOOL_FALSE(
                GetThreadContext(suspendedThread.get(), &c));
            if (c.Eip == m_pRtlUserThreadStart) {
                threadNotStartedYet = true;
            }
        } else {
            _CONTEXT64 c;
            c.ContextFlags = CONTEXT64_CONTROL;
            THROW_IF_WIN32_BOOL_FALSE(
                GetThreadContext64(suspendedThread.get(), &c));
            if (c.Rip == m_pRtlUserThreadStart) {
                threadNotStartedYet = true;
            }
        }
#endif  // _WIN64

        if (threadNotStartedYet) {
            wil::unique_mutex_nothrow mutex(
                CreateProcessInitAPCMutex(dwProcessId, TRUE));
            if (GetLastError() == ERROR_ALREADY_EXISTS) {
                return;  // APC was already created
            }

            auto mutexLock = mutex.ReleaseMutex_scope_exit();

            DllInject::DllInject(hProcess, suspendedThread.get(),
                                 GetCurrentProcess(), mutex.get(),
                                 threadAttachExempt);
            VERBOSE(L"DllInject succeeded for new process %u via APC",
                    dwProcessId);

            return;
        }
    }

    wil::unique_mutex_nothrow mutex(
        OpenProcessInitAPCMutex(dwProcessId, SYNCHRONIZE));
    if (mutex) {
        return;  // APC was already created
    }

    DllInject::DllInject(hProcess, nullptr, GetCurrentProcess(), nullptr,
                         threadAttachExempt);
    VERBOSE(L"DllInject succeeded for new process %u via a remote thread",
            dwProcessId);
}
