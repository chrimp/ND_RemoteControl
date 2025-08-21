#define UNICODE

#include <windows.h>
#include <tlhelp32.h>
#include <tchar.h>
#include <WtsApi32.h>
#include <string>
#include <TlHelp32.h>

#pragma comment(lib, "Wtsapi32.lib")

SERVICE_STATUS ServiceStatus;
SERVICE_STATUS_HANDLE hStatus;

void ServiceMain(int argc, char** argv);
void ControlHandler(DWORD request);
void RunServiceLogic();
bool IsProcessRunning(const wchar_t* processName);
bool StartProcessAsUser(const wchar_t* processPath, const wchar_t* arguments);
void LogMessage(const char* message);

const wchar_t* processName = L"main_service.exe";

int main() {
    SERVICE_TABLE_ENTRY ServiceTable[] = {
        { (LPWSTR)L"ND_Remote_Test", (LPSERVICE_MAIN_FUNCTION)ServiceMain },
        { NULL, NULL }
    };

    if (!StartServiceCtrlDispatcher(ServiceTable)) {
        LogMessage("Failed to start service control dispatcher.");
        return GetLastError();
    }

    return 0;
}

void ServiceMain(int argc, char** argv) {
    hStatus = RegisterServiceCtrlHandler(L"ND_Remote_Test", (LPHANDLER_FUNCTION)ControlHandler);
    if (hStatus == NULL) {
        LogMessage("Failed to register service control handler.");
        return;
    }

    ServiceStatus.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    ServiceStatus.dwCurrentState = SERVICE_START_PENDING;
    ServiceStatus.dwControlsAccepted = SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN;
    ServiceStatus.dwWin32ExitCode = 0;
    ServiceStatus.dwServiceSpecificExitCode = 0;
    ServiceStatus.dwCheckPoint = 0;
    ServiceStatus.dwWaitHint = 0;

    SetServiceStatus(hStatus, &ServiceStatus);

    // Service is now running
    ServiceStatus.dwCurrentState = SERVICE_RUNNING;
    SetServiceStatus(hStatus, &ServiceStatus);

    LogMessage("Service started successfully.");
    RunServiceLogic();
}

void ControlHandler(DWORD request) {
    switch (request) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
    {
        // kill main_service.exe
        HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        PROCESSENTRY32W pEntry;
        pEntry.dwSize = sizeof(pEntry);
        BOOL hRes = Process32FirstW(hSnapshot, &pEntry);

        while (hRes) {
            if (_wcsicmp(pEntry.szExeFile, processName) == 0) {
                HANDLE hProcess = OpenProcess(PROCESS_TERMINATE, FALSE, pEntry.th32ProcessID);
                if (hProcess) {
                    TerminateProcess(hProcess, 0);
                    CloseHandle(hProcess);
                }
                break;
            }
            hRes = Process32NextW(hSnapshot, &pEntry);
        }

        CloseHandle(hSnapshot);
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

void RunServiceLogic() {
    std::wstring processPath = L"C:\\NDR\\" + std::wstring(processName);
    //const wchar_t* processPath = L"C:\\NDR\\main_service.exe"
    const wchar_t* arguments = L"-c 192.168.10.1 192.168.10.20 R"; // Put your actual arguments here

    while (ServiceStatus.dwCurrentState == SERVICE_RUNNING) {
        if (!IsProcessRunning(processName)) {
            LogMessage("main_service.exe is not running. Attempting to restart...");
            if (!StartProcessAsUser(processPath.c_str(), arguments)) {
                LogMessage("Failed to start main_service.exe.");
            } else {
                LogMessage("Successfully started main_service.exe.");
            }
        }

        Sleep(2000); // Check every 1 second
    }
}

bool IsProcessRunning(const wchar_t* processName) {
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) {
        LogMessage("Failed to create process snapshot.");
        return false;
    }

    PROCESSENTRY32 pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32);

    if (Process32First(hSnapshot, &pe32)) {
        do {
            if (_wcsicmp(pe32.szExeFile, processName) == 0) {
                CloseHandle(hSnapshot);
                return true;
            }
        } while (Process32Next(hSnapshot, &pe32));
    }

    CloseHandle(hSnapshot);
    return false;
}

bool StartProcessAsUser(const wchar_t* processPath, const wchar_t* arguments) {
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    HANDLE hToken = NULL;

    auto EnablePriv = [](LPCWSTR name) {
        HANDLE tok;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok)) return;

        LUID luid;
        if (!LookupPrivilegeValueW(nullptr, name, &luid)) {
            CloseHandle(tok);
            return;
        }
        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Luid = luid;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), nullptr, nullptr);
        CloseHandle(tok);
    };
    EnablePriv(SE_ASSIGNPRIMARYTOKEN_NAME);
    EnablePriv(SE_INCREASE_QUOTA_NAME);
    EnablePriv(SE_TCB_NAME);

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_DUPLICATE | TOKEN_QUERY | TOKEN_ASSIGN_PRIMARY, &hToken)) {
        LogMessage("Failed to open process token for SYSTEM.");
        return false;
    }

    HANDLE hDupToken = NULL;
    if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, NULL, SecurityImpersonation, TokenPrimary, &hDupToken)) {
        CloseHandle(hToken);
        LogMessage("Failed to duplicate user token.");
        return false;
    }

    if (sessionId != 0xFFFFFFFF) {
        SetTokenInformation(hDupToken, TokenSessionId, &sessionId, sizeof(sessionId));
    }

    HANDLE hStdOutRead, hStdOutWrite;
    HANDLE hStdErrRead, hStdErrWrite;

    SECURITY_ATTRIBUTES sa = { sizeof(SECURITY_ATTRIBUTES), NULL, TRUE };
    if (!CreatePipe(&hStdOutRead, &hStdOutWrite, &sa, 0) ||
        !CreatePipe(&hStdErrRead, &hStdErrWrite, &sa, 0)) {
        CloseHandle(hToken);
        CloseHandle(hDupToken);
        LogMessage("Failed to create pipes for stdout and stderr.");
        return false;
    }

    SetHandleInformation(hStdOutRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(hStdErrRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOEX siEx = { 0 };
    siEx.StartupInfo.cb = sizeof(STARTUPINFO);
    siEx.StartupInfo.lpDesktop = (LPWSTR)L"winsta0\\default";
    siEx.StartupInfo.hStdError = hStdErrWrite;
    siEx.StartupInfo.hStdOutput = hStdOutWrite;
    siEx.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;

    SIZE_T attributeListSize = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &attributeListSize);
    siEx.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)HeapAlloc(GetProcessHeap(), 0, attributeListSize);
    if (!siEx.lpAttributeList || !InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &attributeListSize)) {
        CloseHandle(hDupToken);
        CloseHandle(hStdOutRead);
        CloseHandle(hStdOutWrite);
        CloseHandle(hStdErrRead);
        CloseHandle(hStdErrWrite);
        if (siEx.lpAttributeList) HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);
        LogMessage("Failed to initialize attribute list.");
        return false;
    }

    HANDLE handlesToInherit[] = { hStdOutWrite, hStdErrWrite };
    if (!UpdateProcThreadAttribute(siEx.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handlesToInherit, sizeof(handlesToInherit), NULL, NULL)) {
        DeleteProcThreadAttributeList(siEx.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);
        CloseHandle(hDupToken);
        CloseHandle(hStdOutRead);
        CloseHandle(hStdOutWrite);
        CloseHandle(hStdErrRead);
        CloseHandle(hStdErrWrite);
        LogMessage("Failed to update attribute list with handles.");
        return false;
    }

    PROCESS_INFORMATION pi = { 0 };

    wchar_t commandLine[MAX_PATH];
    swprintf_s(commandLine, MAX_PATH, L"\"%s\" %s", processPath, arguments);

    BOOL result = CreateProcessAsUserW(
        hDupToken,
        processPath,
        commandLine,
        NULL,
        NULL,
        TRUE,
        CREATE_UNICODE_ENVIRONMENT | CREATE_NO_WINDOW | DETACHED_PROCESS,
        NULL,
        L"C:\\NDR",
        &siEx.StartupInfo,
        &pi
    );

    CloseHandle(hStdOutWrite);
    CloseHandle(hStdErrWrite);
    CloseHandle(hToken);
    CloseHandle(hDupToken);

    if (!result) {
        CloseHandle(hStdOutRead);
        CloseHandle(hStdErrRead);
        std::string reason = "CreateProcessAsUser failed with error: " + std::to_string(GetLastError());
        LogMessage("Failed to create process as user.");
        LogMessage(reason.c_str());
        return false;
    }

    // Wait for process to exit so its stdio buffers flush
    WaitForSingleObject(pi.hProcess, INFINITE);

    HANDLE hFile = CreateFile(L"C:\\NDR\\main.log", GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        SetFilePointer(hFile, 0, NULL, FILE_END);

        char buffer[1024];
        DWORD bytesRead, bytesWritten;

        while (ReadFile(hStdOutRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            SYSTEMTIME st; GetLocalTime(&st);
            char timestamp[64];
            sprintf_s(timestamp, "%04d-%02d-%02d %02d:%02d:%02d.%03d: ",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
            WriteFile(hFile, timestamp, (DWORD)strlen(timestamp), &bytesWritten, NULL);
            WriteFile(hFile, buffer, bytesRead, &bytesWritten, NULL);
        }

        while (ReadFile(hStdErrRead, buffer, sizeof(buffer) - 1, &bytesRead, NULL) && bytesRead > 0) {
            buffer[bytesRead] = '\0';
            SYSTEMTIME st; GetLocalTime(&st);
            char timestamp[64];
            sprintf_s(timestamp, "%04d-%02d-%02d %02d:%02d:%02d.%03d: ",
                      st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
            WriteFile(hFile, timestamp, (DWORD)strlen(timestamp), &bytesWritten, NULL);
            WriteFile(hFile, buffer, bytesRead, &bytesWritten, NULL);
        }
        CloseHandle(hFile);
    }

    CloseHandle(hStdOutRead);
    CloseHandle(hStdErrRead);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return true;
}

void LogMessage(const char* message) {
    const wchar_t* logFilePath = L"C:\\NDR\\service.log"; // Full path to the log file

    DWORD fileAttributes = GetFileAttributes(logFilePath);
    if (fileAttributes == INVALID_FILE_ATTRIBUTES) {
        // File does not exist, create it
        HANDLE hFile = CreateFile(
            logFilePath,
            GENERIC_WRITE,      // Open for writing
            FILE_SHARE_READ,    // Allow other processes to read
            NULL,
            CREATE_NEW,         // Create the file only if it doesn't exist
            FILE_ATTRIBUTE_NORMAL,
            NULL
        );

        if (hFile != INVALID_HANDLE_VALUE) {
            CloseHandle(hFile); // Close the handle after creating the file
        }
    }

    // Open the file for appending
    HANDLE hFile = CreateFile(
        logFilePath,
        FILE_APPEND_DATA, // Open for appending
        FILE_SHARE_READ,  // Allow other processes to read
        NULL,
        OPEN_EXISTING,    // Open the file only if it exists
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (hFile != INVALID_HANDLE_VALUE) {
        SYSTEMTIME st;
        GetLocalTime(&st);

        // Prepare the timestamp
        char timestamp[64];
        sprintf_s(timestamp, "[%04d-%02d-%02d %02d:%02d:%02d] ",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

        // Write the timestamp and message to the log file
        DWORD bytesWritten;
        WriteFile(hFile, timestamp, strlen(timestamp), &bytesWritten, NULL);
        WriteFile(hFile, message, strlen(message), &bytesWritten, NULL);
        WriteFile(hFile, "\r\n", 2, &bytesWritten, NULL); // Add a newline

        CloseHandle(hFile);
    }
}