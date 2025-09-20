#define UNICODE
#include <windows.h>
#include <tlhelp32.h>
#include <WtsApi32.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>

#pragma comment(lib, "Wtsapi32.lib")

#define ENABLE_STDIO_PIPING 1

static const wchar_t* kServiceName = L"ND_Remote_Test";
static const wchar_t* processName  = L"main_service.exe";

SERVICE_STATUS         ServiceStatus;
SERVICE_STATUS_HANDLE  hStatus;
static std::wstring    g_ChildArguments; // REQUIRED (service stops if empty)

void WINAPI ServiceMain(DWORD argc, LPWSTR* argv);
void ControlHandler(DWORD request);
void RunServiceLogic();
bool IsProcessRunning(const wchar_t* processName);
bool StartProcessAsUser(const wchar_t* processPath, const wchar_t* arguments);
void LogMessage(const char* message);

// ---------- Logging ----------
void LogMessage(const char* message) {
    static std::mutex logMutex;
    std::lock_guard<std::mutex> lock(logMutex);
    const wchar_t* logFilePath = L"C:\\NDR\\service.log";

    DWORD attrs = GetFileAttributesW(logFilePath);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        HANDLE hNew = CreateFileW(logFilePath, GENERIC_WRITE, FILE_SHARE_READ, NULL,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (hNew != INVALID_HANDLE_VALUE) CloseHandle(hNew);
    }

    HANDLE hFile = CreateFileW(logFilePath, FILE_APPEND_DATA, FILE_SHARE_READ, NULL,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st; GetLocalTime(&st);
    char ts[64];
    sprintf_s(ts, "[%04d-%02d-%02d %02d:%02d:%02d.%03d] ",
              st.wYear, st.wMonth, st.wDay,
              st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

    DWORD written;
    WriteFile(hFile, ts, (DWORD)strlen(ts), &written, NULL);
    WriteFile(hFile, message, (DWORD)strlen(message), &written, NULL);
    WriteFile(hFile, "\r\n", 2, &written, NULL);
    CloseHandle(hFile);
}

// ---------- Registry argument load (Parameters key) ----------
static bool LoadRegistryArguments(std::wstring& outArgs) {
    outArgs.clear();
    std::wstring baseKey = L"SYSTEM\\CurrentControlSet\\Services\\";
    baseKey += kServiceName;
    baseKey += L"\\Parameters";
    HKEY hKey;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, baseKey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS)
        return false;

    auto TryValue = [&](const wchar_t* valueName) -> bool {
        DWORD type = 0;
        DWORD bytes = 0;
        if (RegQueryValueExW(hKey, valueName, NULL, &type, NULL, &bytes) != ERROR_SUCCESS ||
            (type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t))
            return false;
        std::vector<wchar_t> buf(bytes / sizeof(wchar_t) + 1, 0);
        if (RegQueryValueExW(hKey, valueName, NULL, &type, (LPBYTE)buf.data(), &bytes) != ERROR_SUCCESS)
            return false;
        outArgs.assign(buf.data());
        return !outArgs.empty();
    };

    bool ok = TryValue(L"Arguments") || TryValue(L"Args") || TryValue(L"CommandLine");
    RegCloseKey(hKey);
    return ok;
}

// ---------- Merge / choose argument sources ----------
static bool CollectChildArguments(DWORD svcArgc, LPWSTR* svcArgv) {
    g_ChildArguments.clear();

    // 1) Runtime start parameters (Services.msc "Start parameters" or 'sc start svc arg1 arg2')
    if (svcArgc > 1) {
        for (DWORD i = 1; i < svcArgc; ++i) {
            if (!g_ChildArguments.empty()) g_ChildArguments += L" ";
            g_ChildArguments += svcArgv[i];
        }
        LogMessage("Using arguments from service start parameters.");
        return true;
    }

    // 2) Arguments appended to binPath (parse full process command line)
    {
        LPWSTR fullCmd = GetCommandLineW();
        int cmdArgc = 0;
        LPWSTR* cmdArgv = CommandLineToArgvW(fullCmd, &cmdArgc);
        if (cmdArgv) {
            if (cmdArgc > 1) {
                // Skip first (exe path). Take the rest as candidate binPath args.
                std::wstring tmp;
                for (int i = 1; i < cmdArgc; ++i) {
                    if (!tmp.empty()) tmp += L" ";
                    tmp += cmdArgv[i];
                }
                // BUT avoid duplicating: If SCM provided runtime params, they already handled above.
                if (!tmp.empty()) {
                    g_ChildArguments = tmp;
                    LogMessage("Using arguments from binPath command line.");
                    LocalFree(cmdArgv);
                    return true;
                }
            }
            LocalFree(cmdArgv);
        }
    }

    // 3) Registry Parameters key
    {
        std::wstring regArgs;
        if (LoadRegistryArguments(regArgs) && !regArgs.empty()) {
            g_ChildArguments = regArgs;
            LogMessage("Using arguments from registry Parameters key.");
            return true;
        }
    }

    LogMessage("No arguments found (runtime/binPath/registry).");
    return false;
}

// ---------- Entry ----------
int wmain() {
    SERVICE_TABLE_ENTRYW table[] = {
        { const_cast<LPWSTR>(kServiceName), (LPSERVICE_MAIN_FUNCTIONW)ServiceMain },
        { NULL, NULL }
    };
    if (!StartServiceCtrlDispatcherW(table)) {
        LogMessage("Failed to start service control dispatcher.");
        return (int)GetLastError();
    }
    return 0;
}

// ---------- ServiceMain ----------
void WINAPI ServiceMain(DWORD argc, LPWSTR* argv) {
    hStatus = RegisterServiceCtrlHandlerW(kServiceName, (LPHANDLER_FUNCTION)ControlHandler);
    if (!hStatus) {
        LogMessage("RegisterServiceCtrlHandler failed.");
        return;
    }

    ServiceStatus = {};
    ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    SetServiceStatus(hStatus, &ServiceStatus);

    if (!CollectChildArguments(argc, argv) || g_ChildArguments.empty()) {
        LogMessage("No valid arguments. Stopping service.");
        ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        ServiceStatus.dwWin32ExitCode = ERROR_INVALID_PARAMETER;
        SetServiceStatus(hStatus, &ServiceStatus);
        return;
    }

    ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(hStatus, &ServiceStatus);
    LogMessage("Service running.");
    RunServiceLogic();
}

// ---------- Control Handler ----------
void ControlHandler(DWORD request) {
    switch (request) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN: {
        HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snap != INVALID_HANDLE_VALUE) {
            PROCESSENTRY32W pe{ sizeof(pe) };
            if (Process32FirstW(snap, &pe)) {
                do {
                    if (_wcsicmp(pe.szExeFile, processName) == 0) {
                        HANDLE hp = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                        if (hp) { TerminateProcess(hp, 0); CloseHandle(hp); }
                        break;
                    }
                } while (Process32NextW(snap, &pe));
            }
            CloseHandle(snap);
        }
        ServiceStatus.dwCurrentState = SERVICE_STOPPED;
        SetServiceStatus(hStatus, &ServiceStatus);
        LogMessage("Service stopped.");
        return;
    }
    default:
        break;
    }
    SetServiceStatus(hStatus, &ServiceStatus);
}

// ---------- Monitor loop ----------
void RunServiceLogic() {
    std::wstring processPath = std::wstring(L"C:\\NDR\\") + processName;
    while (ServiceStatus.dwCurrentState == SERVICE_RUNNING) {
        if (!IsProcessRunning(processName)) {
            LogMessage("Child not running. Starting...");
            if (!StartProcessAsUser(processPath.c_str(), g_ChildArguments.c_str()))
                LogMessage("Child start failed.");
            else
                LogMessage("Child started.");
        }
        Sleep(2000);
    }
}

// ---------- Process check ----------
bool IsProcessRunning(const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        LogMessage("Snapshot failed.");
        return false;
    }
    PROCESSENTRY32W pe{ sizeof(pe) };
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, name) == 0) {
                CloseHandle(snap);
                return true;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return false;
}

#if ENABLE_STDIO_PIPING
static void PipeReaderThread(HANDLE hRead) {
    if (!hRead) return;
    std::string line;
    char buf[1024];
    DWORD n = 0;
    while (ReadFile(hRead, buf, sizeof(buf), &n, NULL) && n) {
        for (DWORD i = 0; i < n; ++i) {
            char c = buf[i];
            if (c == '\n' || c == '\r') {
                if (!line.empty()) {
                    LogMessage(line.c_str());
                    line.clear();
                } else {
                    LogMessage("");
                }
            } else {
                line.push_back(c);
                if (line.size() > 8192) {
                    LogMessage(line.c_str());
                    line.clear();
                }
            }
        }
    }
    if (!line.empty()) LogMessage(line.c_str());
    CloseHandle(hRead);
}
#endif

// ---------- Start child ----------
bool StartProcessAsUser(const wchar_t* processPath, const wchar_t* arguments) {
    if (!arguments || !*arguments) {
        LogMessage("StartProcessAsUser: empty args.");
        return false;
    }

    DWORD sessionId = WTSGetActiveConsoleSessionId();
    HANDLE hTok = NULL;
    auto EnablePriv = [](LPCWSTR name) {
        HANDLE tok;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return;
        LUID luid;
        if (!LookupPrivilegeValueW(NULL, name, &luid)) { CloseHandle(tok); return; }
        TOKEN_PRIVILEGES tp{};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL);
        CloseHandle(tok);
    };
    EnablePriv(SE_ASSIGNPRIMARYTOKEN_NAME);
    EnablePriv(SE_INCREASE_QUOTA_NAME);
    EnablePriv(SE_TCB_NAME);

    if (!OpenProcessToken(GetCurrentProcess(),
        TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY,
        &hTok)) {
        LogMessage("OpenProcessToken failed.");
        return false;
    }

    HANDLE hDup = NULL;
    if (!DuplicateTokenEx(hTok, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &hDup)) {
        CloseHandle(hTok);
        LogMessage("DuplicateTokenEx failed.");
        return false;
    }
    if (sessionId != 0xFFFFFFFF)
        SetTokenInformation(hDup, TokenSessionId, &sessionId, sizeof(sessionId));

#if ENABLE_STDIO_PIPING
    HANDLE outR = NULL, outW = NULL;
    HANDLE errR = NULL, errW = NULL;
    SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    if (!CreatePipe(&outR, &outW, &sa, 0) || !CreatePipe(&errR, &errW, &sa, 0)) {
        CloseHandle(hTok); CloseHandle(hDup);
        LogMessage("CreatePipe failed.");
        return false;
    }
    SetHandleInformation(outR, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errR, HANDLE_FLAG_INHERIT, 0);
#endif

    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof(STARTUPINFOW);
    si.StartupInfo.lpDesktop = const_cast<LPWSTR>(L"winsta0\\default");
#if ENABLE_STDIO_PIPING
    si.StartupInfo.hStdOutput = outW;
    si.StartupInfo.hStdError  = errW;
    si.StartupInfo.dwFlags   |= STARTF_USESTDHANDLES;
#endif

#if ENABLE_STDIO_PIPING
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attrSize);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attrSize);
    if (!si.lpAttributeList ||
        !InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize)) {
        if (si.lpAttributeList) HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
        CloseHandle(hDup); CloseHandle(hTok);
        CloseHandle(outR); CloseHandle(outW);
        CloseHandle(errR); CloseHandle(errW);
        LogMessage("Init attribute list failed.");
        return false;
    }
    HANDLE inherit[] = { outW, errW };
    if (!UpdateProcThreadAttribute(si.lpAttributeList, 0,
                                   PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherit, sizeof(inherit), NULL, NULL)) {
        DeleteProcThreadAttributeList(si.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
        CloseHandle(hDup); CloseHandle(hTok);
        CloseHandle(outR); CloseHandle(outW);
        CloseHandle(errR); CloseHandle(errW);
        LogMessage("UpdateProcThreadAttribute failed.");
        return false;
    }
#endif

    std::wstring cmd = L"\"" + std::wstring(processPath) + L"\" ";
    cmd += arguments;
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessAsUserW(
        hDup,
        processPath,
        cmdBuf.data(),
        NULL,
        NULL,
#if ENABLE_STDIO_PIPING
        TRUE,
#else
        FALSE,
#endif
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | DETACHED_PROCESS,
        NULL,
        L"C:\\NDR",
        &si.StartupInfo,
        &pi
    );

#if ENABLE_STDIO_PIPING
    CloseHandle(outW);
    CloseHandle(errW);
#endif
    CloseHandle(hTok);
    CloseHandle(hDup);

    if (!ok) {
#if ENABLE_STDIO_PIPING
        CloseHandle(outR);
        CloseHandle(errR);
#endif
        if (si.lpAttributeList) {
            DeleteProcThreadAttributeList(si.lpAttributeList);
            HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
        }
        std::string err = "CreateProcessAsUser failed err=" + std::to_string(GetLastError());
        LogMessage(err.c_str());
        return false;
    }

#if ENABLE_STDIO_PIPING
    std::thread tOut(PipeReaderThread, outR);
    std::thread tErr(PipeReaderThread, errR);
#endif

    WaitForSingleObject(pi.hProcess, INFINITE);

#if ENABLE_STDIO_PIPING
    if (tOut.joinable()) tOut.join();
    if (tErr.joinable()) tErr.join();
#endif

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

#if ENABLE_STDIO_PIPING
    if (si.lpAttributeList) {
        DeleteProcThreadAttributeList(si.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, si.lpAttributeList);
    }
#endif
    return true;
}