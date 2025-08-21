#include "InputNDSession.hpp"

#include <emmintrin.h>
#include <exception>
#include <ios>
#include <windows.h>
#include <winnt.h>
#include <winuser.h>

#ifdef _DEBUG
#undef FAILED
    #define FAILED(hr) \
        ((hr) < 0 ? (std::cout << std::hex << hr, throw std::exception(), true) : false)
#endif

#pragma comment(lib, "synchronization.lib")

constexpr char TEST_PORT[] = "54322";

extern std::atomic<bool> g_shouldQuit;

bool InputNDSessionServer::Setup(char* localAddr) {
    if (!Initialize(localAddr)) std::terminate();

    ND2_ADAPTER_INFO info = GetAdapterInfo();
    if (info.AdapterId == 0) std::terminate();

    m_MaxSge = info.MaxInitiatorSge;

    if (FAILED(CreateCQ(info.MaxCompletionQueueDepth))) std::terminate();
    if (FAILED(CreateQP(info.MaxReceiveQueueDepth, info.MaxInitiatorQueueDepth, info.MaxReceiveSge, info.MaxInitiatorSge))) std::terminate();
    if (FAILED(CreateMR())) std::terminate();

    ULONG flags = ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE;
    if (FAILED(RegisterDataBuffer(INPUT_EVENT_BUFFER_SIZE, flags))) std::terminate();

    if (FAILED(CreateListener())) std::terminate();
    if (FAILED(CreateConnector())) std::terminate();

    return true;
}

void InputNDSessionServer::OpenListener(const char* localAddr) {
    char fullAddress[INET_ADDRSTRLEN + 6];
    sprintf_s(fullAddress, "%s:%s", localAddr, TEST_PORT);
    
    std::cout << "INPUT: Listening on " << fullAddress << "..." << std::endl;
    if (FAILED(Listen(fullAddress))) std::terminate();

    if (FAILED(GetConnectionRequest())) {
        std::cerr << "INPUT: " << "GetConnectionRequest failed. Reason: " << std::hex << GetResult() << std::endl;
        return;
    }

    if (FAILED(Accept(1, 1, nullptr, 0))) std::terminate();

    CreateMW();
    Bind(m_Buf, INPUT_EVENT_BUFFER_SIZE, ND_OP_FLAG_ALLOW_WRITE | ND_OP_FLAG_ALLOW_READ);
}

void InputNDSessionServer::ExchangePeerInfo() {
    PeerInfo myInfo = { reinterpret_cast<UINT64>(m_Buf), m_pMw->GetRemoteToken() };

    ND2_SGE sge = {m_Buf, sizeof(PeerInfo), m_pMr->GetLocalToken()};
    if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
        std::cerr << "INPUT: " << "PostReceive for PeerInfo failed." << std::endl;
        return;
    }

    if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
        std::cerr << "INPUT: " << "WaitForCompletion for PeerInfo failed." << std::endl;
        return;
    }

    remoteInfo.remoteAddr = reinterpret_cast<PeerInfo*>(m_Buf)->remoteAddr;
    remoteInfo.remoteToken = reinterpret_cast<PeerInfo*>(m_Buf)->remoteToken;

    memset(m_Buf, 0, INPUT_EVENT_BUFFER_SIZE);
    memcpy(m_Buf, &myInfo, sizeof(PeerInfo));

    std::this_thread::sleep_for(std::chrono::seconds(1));

    if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
        std::cerr << "INPUT: " << "Send for PeerInfo failed." << std::endl;
        return;
    }
    if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
        std::cerr << "INPUT: " << "WaitForCompletion for PeerInfo send failed." << std::endl;
        return;
    }

    memset(m_Buf, 0, INPUT_EVENT_BUFFER_SIZE);
}

static bool isAlt = false;
static bool isTab = false;
static bool isLastAlt = false;

static UINT KeyFlags = 0;

void InputNDSessionServer::SendEvent(RAWINPUT input) {
    switch (input.header.dwType) {
        case RIM_TYPEMOUSE: {
            #ifdef ABSCURSOR
            m_Mouse.x.store(static_cast<short>(input.data.mouse.lLastX));
            m_Mouse.y.store(static_cast<short>(input.data.mouse.lLastY));
            #else
            m_Mouse.x.fetch_add(static_cast<short>(input.data.mouse.lLastX));
            m_Mouse.y.fetch_add(static_cast<short>(input.data.mouse.lLastY));
            #endif
            m_Mouse.wheel.fetch_add(input.data.mouse.usButtonData);
            m_Mouse.buttonFlags.fetch_or(input.data.mouse.ulButtons); // Let receiver handle
            m_Mouse.absolute.store(input.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE);

            break;
        }

        case RIM_TYPEKEYBOARD: {
            switch (input.data.keyboard.MakeCode) {
                case 0x38: // Alt key
                    if (input.data.keyboard.Flags & RI_KEY_BREAK) {
                        KeyFlags &= ~0x38;
                    } else {
                        KeyFlags |= 0x38;
                    }
                    break;
                case 0x0F: // Tab key
                    if (input.data.keyboard.Flags & RI_KEY_BREAK) {
                        KeyFlags &= ~0x0F;
                    } else {
                        KeyFlags |= 0x0F;
                    }
                    break;
                case 0x1D: // Ctrl key
                    if (input.data.keyboard.Flags & RI_KEY_BREAK) {
                        KeyFlags &= ~0x1D;
                    } else {
                        KeyFlags |= 0x1D;
                    }
                    break;
                case 0x2A: // Left Shift key
                case 0x36: // Right Shift key
                    if (input.data.keyboard.Flags & RI_KEY_BREAK) {
                        KeyFlags &= ~(0x2A | 0x36); // Clear both shift flags
                    } else {
                        KeyFlags |= (input.data.keyboard.MakeCode == 0x2A ? 0x2A : 0x36);
                    }
                    break;
                case 0x2D: // X key
                    if (input.data.keyboard.Flags & RI_KEY_BREAK) {
                        KeyFlags &= ~0x2D;
                    } else {
                        KeyFlags |= 0x2D;
                    }
                    break;
            }

            // Check for Alt+Tab
            if ((KeyFlags & 0x38) && (KeyFlags & 0x0F) && input.data.keyboard.MakeCode == 0x0F && 
                !(input.data.keyboard.Flags & RI_KEY_BREAK)) {
                
                std::cout << "INPUT: Alt+Tab detected, blocking" << std::endl;
                
                m_Keyboard.scanCode.store(input.data.keyboard.MakeCode);
                m_Keyboard.down.store(3); // Special value to indicate blocked combination
                m_Keyboard.isE0.store((input.data.keyboard.Flags & RI_KEY_E0) != 0);
                
                // Unclip & show cursor
                ClipCursor(nullptr);
                while (ShowCursor(true) < 0);
                return;
            }

            // Check for Ctrl+Alt+Shift+X
            if ((KeyFlags & 0x1D) && (KeyFlags & 0x38) && (KeyFlags & (0x2A | 0x36)) && (KeyFlags & 0x2D) &&
                input.data.keyboard.MakeCode == 0x2D && !(input.data.keyboard.Flags & RI_KEY_BREAK)) {
                
                std::cout << "INPUT: Ctrl+Alt+Shift+X detected, blocking" << std::endl;
                
                m_Keyboard.scanCode.store(input.data.keyboard.MakeCode);
                m_Keyboard.down.store(3); // Special value to indicate blocked combination
                m_Keyboard.isE0.store((input.data.keyboard.Flags & RI_KEY_E0) != 0);

                ClipCursor(nullptr);
                while (ShowCursor(true) < 0);
                
                return;
            }

            m_Keyboard.scanCode.store(input.data.keyboard.MakeCode);
            
            switch (input.data.keyboard.Flags) {
                case RI_KEY_MAKE:
                    m_Keyboard.down.store(0); // Key down
                    break;
                case RI_KEY_BREAK:
                    m_Keyboard.down.store(1); // Key up
                    break;
                case RI_KEY_E0:
                case RI_KEY_E1:
                    m_Keyboard.down.store(0);
                    break;
                case RI_KEY_E0 + 1:
                case RI_KEY_E1 + 1:
                    m_Keyboard.down.store(1);
                    break;
                default:
                    m_Keyboard.down.store(2); // No change
                    break;
            }

            break;
        }

        default:
            break;
    }
}

void InputNDSessionServer::Loop() {
    Packet* packet = reinterpret_cast<Packet*>(reinterpret_cast<uint8_t*>(m_Buf) + 1);
    packet->mouse = { 0, 0, 0, 0 };
    packet->key = { 0, 2 }; // 2 - nothing

    ND2_SGE flagSge = { m_Buf, 1, m_pMr->GetLocalToken() };
    ND2_SGE sge = { m_Buf, sizeof(Packet), m_pMr->GetLocalToken() };
    volatile uint8_t* flag = reinterpret_cast<uint8_t*>(m_Buf);
    flag[0] = 0; // Reset flag
    _mm_clflush(m_Buf);
    _mm_sfence();
    
    // m_Mouse now accumulates mouse deltas, so no need to calculate delta. Just reset it.

    auto flagWaitTotal = std::chrono::microseconds(0);

    while (m_isRunning && !g_shouldQuit.load()) {
        flag[0] = 0; // Reset flag
        _mm_clflush(m_Buf);
        _mm_sfence();

        WaitForSingleObject(m_hCallbackEvent, INFINITE);
        //delta = {m_Mouse.x.exchange(0), m_Mouse.y.exchange(0)};

        packet->mouse = {
            m_Mouse.x.exchange(0),
            m_Mouse.y.exchange(0),
            m_Mouse.wheel.exchange(0),
            m_Mouse.absolute.exchange(false),
            m_Mouse.buttonFlags.exchange(0)
        };

        packet->key = {
            m_Keyboard.scanCode.exchange(0),
            m_Keyboard.down.exchange(2)
        };

        auto flagWaitStart = std::chrono::steady_clock::now();
        while (flag[0] != 1) {
            if (FAILED(Read(&flagSge, 1, remoteInfo.remoteAddr, remoteInfo.remoteToken, 0, READ_CTXT))) {
                std::cerr << "INPUT: " << "Read failed." << std::endl;
                return;
            }
            if (!WaitForCompletionAndCheckContext(READ_CTXT)) {
                std::cerr << "INPUT: " << "WaitForCompletion for read failed." << std::endl;
                return;
            }
            _mm_clflush(m_Buf);
            _mm_lfence();
            _mm_pause();
        }

        auto flagWaitEnd = std::chrono::steady_clock::now();
        flagWaitTotal += std::chrono::duration_cast<std::chrono::microseconds>(flagWaitEnd - flagWaitStart);

        std::cout << "\r                                                                                                       \r";
        std::cout << "Mouse position: " << packet->mouse.x << ", " << packet->mouse.y << std::flush;
    
        //ND2_SGE sge = { m_Buf, INPUT_EVENT_BUFFER_SIZE, m_pMr->GetLocalToken() };
        if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
            std::cerr << "INPUT: " << "Send failed." << std::endl;
            return;
        }
        // Maybe, maybe it's somehow sending before the completion, making the send stack.
        // That well could be the case, because it was failing with ND_IO_TIMEOUT.
        // But I'm calling WaitForCompletionAndCheckContext(), what does it mean that device was not prompt enough to respond to an I/O request?
        // By docs, ND_IO_TIMEOUT means that the queuepair has failed or the connection has failed.
        // If queuepair has failed, then it should also raise an error in the peer, so it should not be the case.
        // If connection has failed... doesn't it throw an exception on the peer side?
        // What happens if I plug out the cable? I gotta see.
        // So the program doesn't know a thing even if the cable is unplugged.
        // But if I plug it back, it would take some time, then sender throws the same ND_IO_TIMEOUT.
        // But realistically, I've pulled like 20 microseconds per loop for send/recv testing, how a few KHz can break it?
        // I need to see the error code when Send() without PostReceive().
        // Yes, it's ND_IO_TIMEOUT, the same one.

        // Flagging the Send with ND_OP_FLAG_INLINE like, definitely causes the issue.
        // Technically, the size of the packet is 16 bytes, and max inline size is 800 something, and inline operation should always be faster, so it should not be the issue.
        // But it is. Maybe it's just slow and letting DMA is more faster?
        // Maybe it's just friggin' hot to do things itself, I don't have fan attached to the card.

        flag[0] = 0;
        _mm_clflush(m_Buf);
        _mm_sfence();
        if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
            std::cerr << "INPUT: " << "WaitForCompletion for send failed." << std::endl;
            return;
        }
        
        /*
        auto now = std::chrono::steady_clock::now();
        if (now - lastCheck > std::chrono::seconds(1)) {
            std::cout << "\r                                                                               \r" << std::flush;
            std::cout << "INPUT: " << "Flag wait time: " << flagWaitTotal.count() / static_cast<float>(count) << std::flush;
            flagWaitTotal = std::chrono::microseconds(0);
            lastCheck = now;
            count = 0;
        }
        */
    }
    g_shouldQuit.store(true);
}

bool InputNDSessionServer::WaitForCompletionAndCheckContext(void *expectedContext, ULONG notifyFlag) {
    ND2_RESULT ndRes = WaitForCompletion(notifyFlag, true);

    if (ndRes.Status == ND_CANCELED) {
        std::cerr << "INPUT: Remote has closed the connection." << std::endl;
        g_shouldQuit.store(true);
        return false;
    }

    if (ND_SUCCESS != ndRes.Status) {
        std::cerr << "INPUT: " << "Operation failed with status: " << std::hex << ndRes.Status << std::endl;
        #ifdef _DEBUG
        std::terminate();
        #endif
        return false;
    }
    if (expectedContext != ndRes.RequestContext) {
        std::cerr << "INPUT: " << "Unexpected completion. Check for missing WaitForCompletion() call." << std::endl;
        throw std::exception();
        return false;
    }

    return true;
}

bool InputNDSessionClient::Setup(const char* localAddr) {
    if (!Initialize(const_cast<char*>(localAddr))) std::terminate();

    ND2_ADAPTER_INFO info = GetAdapterInfo();
    if (info.AdapterId == 0) std::terminate();

    if (FAILED(CreateCQ(info.MaxCompletionQueueDepth))) std::terminate();
    if (FAILED(CreateQP(info.MaxReceiveQueueDepth, info.MaxInitiatorQueueDepth, info.MaxReceiveSge, info.MaxInitiatorSge))) std::terminate();
    if (FAILED(CreateMR())) std::terminate();

    ULONG flags = ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE;
    if (FAILED(RegisterDataBuffer(INPUT_EVENT_BUFFER_SIZE, flags))) std::terminate();
    if (FAILED(CreateConnector())) std::terminate();

    return true;
}

void InputNDSessionClient::OpenConnector(const char* localAddr, const char* serverAddr) {
    char fullServerAddress[INET_ADDRSTRLEN + 6];
    sprintf_s(fullServerAddress, "%s:%s", serverAddr, TEST_PORT);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    std::cout << "INPUT: Connecting to " << fullServerAddress << "..." << std::endl;

    if (FAILED(Connect(localAddr, fullServerAddress, 1, 1, nullptr, 0))) {
        std::cerr << "INPUT: " << "Connect failed." << std::endl;
        return;
    }
    if (FAILED(CompleteConnect())) {
        std::cerr << "INPUT: " << "CompleteConnect failed." << std::endl;
        return;
    }

    CreateMW();
    Bind(m_Buf, INPUT_EVENT_BUFFER_SIZE, ND_OP_FLAG_ALLOW_WRITE | ND_OP_FLAG_ALLOW_READ);
}

void InputNDSessionClient::ExchangePeerInfo() {
    PeerInfo* myInfo = reinterpret_cast<PeerInfo*>(m_Buf);
    myInfo->remoteAddr = reinterpret_cast<UINT64>(m_Buf);
    myInfo->remoteToken = m_pMw->GetRemoteToken();

    std::this_thread::sleep_for(std::chrono::seconds(1));

    ND2_SGE sge = {m_Buf, sizeof(PeerInfo), m_pMr->GetLocalToken()};
    if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
        std::cerr << "INPUT: " << "Send failed." << std::endl;
        return;
    }
    if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
        std::cerr << "INPUT: " << "WaitForCompletion for PeerInfo send failed." << std::endl;
        return;
    }

    memset(m_Buf, 0, INPUT_EVENT_BUFFER_SIZE);
    sge = {m_Buf, sizeof(PeerInfo), m_pMr->GetLocalToken()};
    if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
        std::cerr << "INPUT: " << "PostReceive for PeerInfo failed." << std::endl;
        return;
    }
    if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
        std::cerr << "INPUT: " << "WaitForCompletion for PeerInfo receive failed." << std::endl;
        return;
    }

    remoteInfo.remoteAddr = reinterpret_cast<PeerInfo*>(m_Buf)->remoteAddr;
    remoteInfo.remoteToken = reinterpret_cast<PeerInfo*>(m_Buf)->remoteToken;

    memset(m_Buf, 0, INPUT_EVENT_BUFFER_SIZE);
}

void LiftAllKeys() {
    INPUT input = {0};
    input.type = INPUT_KEYBOARD;
    input.ki.dwFlags = KEYEVENTF_KEYUP;

    for (int i = 0; i < 2555; ++i) {
        if (GetAsyncKeyState(i) & 0x8000) {
            input.ki.wVk = i;
            SendInput(1, &input, sizeof(INPUT));
        }
    }
}

void InputNDSessionClient::Loop() {
    auto lastprobe = std::chrono::steady_clock::now();
    unsigned int count = 0;
    unsigned char sgeCount = 0;

    ND2_SGE sge = { m_Buf, sizeof(Packet), m_pMr->GetLocalToken() };
    for (int i = 0; i < 20; i++) {
        HRESULT hr = PostReceive(&sge, 1, RECV_CTXT);
        if (FAILED(hr)) {
            std::cerr << "INPUT: " << "PostReceive failed." << std::hex << hr << std::endl;
            return;
        }
    }

    sgeCount += 20;

    uint8_t* flag = reinterpret_cast<uint8_t*>(m_Buf); // 0 = Cannot accomodate 1 = Good to go
    Packet* received = reinterpret_cast<Packet*>(reinterpret_cast<uint8_t*>(m_Buf) + 1); // Buffer for MousePacket

    while (m_isRunning && !g_shouldQuit.load()) {
        if (sgeCount < 10) {
            for (int i = 0; i < 10; i++) {
                if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
                    std::cerr << "INPUT: " << "PostReceive failed." << std::endl;
                    return;
                }
            }
            sgeCount += 10;
        }

        flag[0] = 1;
        _mm_clflush(m_Buf);
        _mm_sfence();

        if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
            std::cerr << "INPUT: " << "WaitForCompletion for PostReceive failed." << std::endl;
            return;
        }
        sgeCount--;

        count++;

        flag[0] = 0;
        _mm_clflush(m_Buf);
        _mm_sfence();

        MousePacket mouse = received->mouse;
        KeyPacket key = received->key;

        bool shouldMouse = (mouse.x != 0 || mouse.y != 0 || mouse.wheel != 0 || mouse.buttonFlags != 0);
        bool shouldKey = (key.scanCode != 0 && key.down != 2);

        if (!shouldMouse && !shouldKey) {
            continue; // Nothing to do
        }

        if (shouldMouse) {
            // Handle mouse
            INPUT input = {0};
            input.type = INPUT_MOUSE;
            input.mi.dx = mouse.x;
            input.mi.dy = mouse.y;

            bool isWheel = (mouse.buttonFlags & RI_MOUSE_WHEEL);
            bool isX = (mouse.buttonFlags & (RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP));
        
            if (isWheel && isX) { // Both can't be set at the same time
                isX = false;
                mouse.buttonFlags &= ~(RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP);
            }

            ULONG xFlag = -1;

            if (isWheel) {
                input.mi.dwFlags = mouse.buttonFlags * 2 | MOUSEEVENTF_MOVE;
                input.mi.mouseData = mouse.wheel;
            } else if (isX) {
                xFlag = mouse.buttonFlags & (RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP);
                input.mi.dwFlags = (mouse.buttonFlags & ~(RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP)) * 2 | MOUSEEVENTF_MOVE;
            } else {
                input.mi.dwFlags = mouse.buttonFlags * 2 | MOUSEEVENTF_MOVE;
                input.mi.mouseData = 0;
            }

            switch (xFlag) {
                case RI_MOUSE_BUTTON_4_DOWN:
                    input.mi.dwFlags |= MOUSEEVENTF_XDOWN;
                    input.mi.mouseData = XBUTTON1;
                    break;
                case RI_MOUSE_BUTTON_5_DOWN:
                    input.mi.dwFlags |= MOUSEEVENTF_XDOWN;
                    input.mi.mouseData = XBUTTON2;
                    break;
                case RI_MOUSE_BUTTON_4_UP:
                    input.mi.dwFlags |= MOUSEEVENTF_XUP;
                    input.mi.mouseData = XBUTTON1;
                    break;
                case RI_MOUSE_BUTTON_5_UP:
                    input.mi.dwFlags |= MOUSEEVENTF_XUP;
                    input.mi.mouseData = XBUTTON2;
                    break;
                default: // This cannot happen, or well could happen if isX is false
                    #ifdef _DEBUG
                    if (isX) throw std::runtime_error("Unexpected X button state.");
                    #endif
                    break;
            }

            std::cout << "\r                                                                              \r";
            std::cout << "INPUT: x: " << mouse.x << ", y: " << mouse.y << " abs: " << std::boolalpha << mouse.absolute << std::flush;

            if (mouse.absolute) {
                input.mi.dwFlags |= MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
                input.mi.dx = (mouse.x * 65536) / GetSystemMetrics(SM_CXSCREEN);
                input.mi.dy = (mouse.y * 65536) / GetSystemMetrics(SM_CYSCREEN);
            } else {
                input.mi.dwFlags |= MOUSEEVENTF_MOVE;
            }

            SetLastError(0);
            if (SendInput(1, &input, sizeof(INPUT)) == 0) {
                DWORD error = GetLastError();
                if (error == ERROR_ACCESS_DENIED || error == 0) {
                    std::cout << "INPUT: SendInput ACCESS_DENIED, attempting desktop change." << std::endl;

                    WCHAR name[256];
                    DWORD needed = 0;
                    HDESK hDesk = nullptr;

                    // Retry open/query loop if OpenInputDesktop fails or GetUserObjectInformationW fails with ERROR_INVALID_HANDLE (6)
                    for (;;) {
                        hDesk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
                        if (!hDesk) {
                            DWORD ge = GetLastError();
                            if (ge == ERROR_INVALID_HANDLE) { Sleep(50); continue; }
                            std::cerr << "INPUT: OpenInputDesktop failed. Error: " << ge << std::endl;
                            break;
                        }
                        if (!GetUserObjectInformationW(hDesk, UOI_NAME, name, sizeof(name), &needed)) {
                            DWORD ge = GetLastError();
                            CloseDesktop(hDesk);
                            hDesk = nullptr;
                            if (ge == ERROR_INVALID_HANDLE) { Sleep(50); continue; }
                            std::cerr << "INPUT: GetUserObjectInformationW failed. Error: " << ge << std::endl;
                            break;
                        }
                        break;
                    }

                    if (hDesk) {
                        CloseDesktop(hDesk);
                        HDESK hNewDesk = OpenDesktopW(name, 0, FALSE, GENERIC_ALL);
                        if (hNewDesk) {
                            if (!SetThreadDesktop(hNewDesk)) {
                                DWORD ge = GetLastError();
                                CloseDesktop(hNewDesk);
                                std::cerr << "INPUT: SetThreadDesktop failed. Error: " << ge << std::endl;
                            } else {
                                CloseDesktop(hNewDesk);
                                SetLastError(0);
                                if (SendInput(1, &input, sizeof(INPUT)) == 0) {
                                    std::cerr << "INPUT: SendInput failed after desktop change: " << GetLastError() << std::endl;
                                    abort();
                                }
                            }
                        } else {
                            std::cerr << "INPUT: OpenDesktopW failed. Error: " << GetLastError() << std::endl;
                        }
                    }
                } else {
                    std::cerr << "INPUT: SendInput failed. Error: " << error << std::endl;
                    abort();
                }
            }
        }
        
        // Handle keyboard
        if (shouldKey) {
            INPUT input = {0};
            input.type = INPUT_KEYBOARD;
            input.ki.wScan = key.scanCode;

            if (key.down == 0) { // Key down
                input.ki.dwFlags = KEYEVENTF_SCANCODE; // Key down
            } else if (key.down == 1) { // Key up
                input.ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP; // Key up
            } else if (key.down == 3) {
                LiftAllKeys();
            } else {
                continue; // No change, skip
            }

            if (key.isE0) {
                input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
            } // E0

            if (SendInput(1, &input, sizeof(INPUT)) == 0) {
                DWORD error = GetLastError();
                if (error == ERROR_ACCESS_DENIED) {
                    std::cout << "INPUT: " << "SendInput failed with ERROR_ACCESS_DENIED, trying to change desktop." << std::endl;
                    HDESK hDesk = OpenInputDesktop(0, FALSE, GENERIC_ALL);
                    WCHAR name[256];
                    DWORD needed = 0;

                    std::wcout << L"INPUT: " << L"Current desktop name: " << std::endl;

                    if (hDesk && GetUserObjectInformationW(hDesk, UOI_NAME, name, sizeof(name), &needed)) {
                        CloseDesktop(hDesk);

                        HDESK hNewDesk = OpenDesktopW(name, 0, FALSE, GENERIC_ALL);
                        SetThreadDesktop(hNewDesk);
                        CloseDesktop(hNewDesk);

                        if (SendInput(1, &input, sizeof(INPUT)) == 0) {
                            std::cerr << "INPUT: " << "SendInput failed after desktop change: " << GetLastError() << std::endl;
                            abort();
                            break;
                        }
                    } else {
                        CloseDesktop(hDesk);
                        std::cerr << "INPUT: " << "Failed to get desktop name. Error: " << GetLastError() << std::endl;
                    }
                } else {
                    std::cerr << "INPUT: " << "SendInput failed with error: " << std::dec << error << std::endl;
                    abort();
                    break;
                }
            }
        }

        /*
        auto now = std::chrono::steady_clock::now();

        if (now - lastprobe >= std::chrono::seconds(1)) {
            std::scoped_lock<std::mutex> lock(m_coutMutex);
            std::cout << "\r                                                       \r" << std::flush;
            std::cout << "INPUT: " << "Input frequency: " << count << " Hz | SGE: " << static_cast<int>(sgeCount) << std::flush;
            lastprobe = now;
            count = 0;
        }
        */
    }
    g_shouldQuit.store(true);
}

bool InputNDSessionClient::WaitForCompletionAndCheckContext(void *expectedContext, ULONG notifyFlag) {
    ND2_RESULT ndRes = WaitForCompletion(notifyFlag, true);

    if (ndRes.Status == ND_CANCELED) {
        std::cout << "INPUT: " << "Remote has closed the connection." << std::endl;
        return false;
    }

    if (ND_SUCCESS != ndRes.Status) {
        std::cerr << "INPUT: " << "Operation failed with status: " << std::hex << ndRes.Status << std::endl;
        #ifdef _DEBUG
        std::terminate();
        #endif
        return false;
    }
    if (expectedContext != ndRes.RequestContext) {
        std::cerr << "INPUT: " << "Unexpected completion. Check for missing WaitForCompletion() call." << std::endl;
        throw std::exception();
        return false;
    }

    return true;
}