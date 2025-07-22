#include "InputNDSession.hpp"

#include <emmintrin.h>
#include <windows.h>

#ifdef _DEBUG
#undef FAILED
    #define FAILED(hr) \
        ((hr) < 0 ? (std::cout << std::hex << hr, throw std::exception(), true) : false)
#endif

#pragma comment(lib, "synchronization.lib")

constexpr char TEST_PORT[] = "54322";


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

void InputNDSessionServer::SendEvent(RAWINPUT input) {
    switch (input.header.dwType) {
        case RIM_TYPEMOUSE: {
            m_Mouse.x.fetch_add(static_cast<short>(input.data.mouse.lLastX));
            m_Mouse.y.fetch_add(static_cast<short>(input.data.mouse.lLastY));
            m_Mouse.wheel.fetch_add(input.data.mouse.usButtonData);
            m_Mouse.buttonFlags.store(input.data.mouse.ulButtons); // Let receiver handle
            break;
        }

        case RIM_TYPEKEYBOARD: {
            switch (input.data.keyboard.VKey) {
                case VK_MENU:
                    if (input.data.keyboard.Flags == RI_KEY_MAKE) {
                        isAlt = true;
                        isLastAlt = true;
                    }
                    else if (input.data.keyboard.Flags == RI_KEY_BREAK) isAlt = false;
                    break;
                case VK_TAB:
                    if (input.data.keyboard.Flags == RI_KEY_MAKE) {
                        isTab = true;
                        isLastAlt = false;
                    }
                    else if (input.data.keyboard.Flags == RI_KEY_BREAK) isTab = false;
                    break;
            }

            if (isAlt && isTab) { // Do not send Alt+Tab
                if (isLastAlt) { // Send Registered key depress
                    m_Keyboard.vk.store(static_cast<unsigned char>(VK_MENU));
                    m_Keyboard.down.store(0);
                } else {
                    m_Keyboard.vk.store(static_cast<unsigned char>(VK_TAB));
                    m_Keyboard.down.store(0);
                }
                isAlt = false;
                isTab = false;
                isLastAlt = false; // Reset last Alt

                return;
            }

            m_Keyboard.vk.store(static_cast<unsigned char>(input.data.keyboard.VKey));
            
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
    auto lastCheck = std::chrono::steady_clock::now();

    unsigned int count = 0;

    while (m_isRunning) {
        flag[0] = 0; // Reset flag
        _mm_clflush(m_Buf);
        _mm_sfence();

        WaitForSingleObject(m_hCallbackEvent, INFINITE);
        //delta = {m_Mouse.x.exchange(0), m_Mouse.y.exchange(0)};

        packet->mouse = {
            m_Mouse.x.exchange(0),
            m_Mouse.y.exchange(0),
            m_Mouse.wheel.exchange(0),
            m_Mouse.buttonFlags.load()
        };

        packet->key = {
            m_Keyboard.vk.load(),
            m_Keyboard.down.load()
        };

        auto flagWaitStart = std::chrono::steady_clock::now();
        count++;
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
}

bool InputNDSessionServer::WaitForCompletionAndCheckContext(void *expectedContext, ULONG notifyFlag) {
    ND2_RESULT ndRes = WaitForCompletion(notifyFlag, true);

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

    while (m_isRunning) {
        if (sgeCount < 10) {
            for (int i = 0; i < 10; i++) {
                if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
                    std::cerr << "INPUT: " << "PostReceive failed." << std::endl;
                    return;
                }
            }
            sgeCount += 10;
        }

        // Let's see this helps with the sync issue.
        // To be honest, I think it should've worked without this, but it doesn't.
        // Which means that with I/O depth of 1, it's exhausting despite the flagging.
        // So if it happens the same, I think this will also eventually fail.
        // Yup, it fails. Man, this is so weird.
        // Okay, it failed without SGE decreasing. This means that Send() is just failing.
        // But it's failing with ND_IO_TIMEOUT, which I think it means that there's no PostReceive().
        // Then what's this for now? What's going on?

        // Wait 20 microseconds to see if it helps
        // It failed at 1KHz on true remote session, so let's try to wait a bit more
        // One cycle for 8KHz is 125 microseconds, and for 1KHz is 1000 microseconds.
        // Still fails at 100 microseconds. RTT is at average like 20-30 microseconds, max is 120 microseconds.
        // So let's try 300 microseconds. Oh god, it still fails.
        /*
        auto waitStart = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - waitStart < std::chrono::microseconds(300)) {
            _mm_pause();
        }
        */
        // It does help, indeed fixes the sync issue.
        // It's like tiny bit of time compared to the max USB 2.0 polling rate.
        // But still, why PostReceive is not *prompt* enough? Why 20 microseconds?
        // Does it have to reach the peer? Then should I wait for the whole RTT to be sure going on?
        // Could use one-sided verb, but spin loop with _mm_pause() fully consumes the core.
        // Flagging with TCP/IP will work, but that will add latency.
        // And iterating TCP/IP for like 1K-8K per second is not a good idea, probably even more expensive.
        // And apparently fencing is not an issue.
        // I do see that InputNDSessionServer reports its flag observation time earlier than InputNDSessionClient,
        // but I do think something's up with the PostReceive() itself.

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
        bool shouldKey = (key.vk != 0 && key.down != 2);

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

            if (SendInput(1, &input, sizeof(INPUT)) == 0) {
                std::cerr << "INPUT: " << "SendInput failed with error: " << GetLastError() << std::endl;
                abort();
                return;
            }
        }
        
        // Handle keyboard
        if (shouldKey) {
            INPUT input = {0};
            input.type = INPUT_KEYBOARD;
            input.ki.wVk = key.vk;

            if (key.down == 0) { // Key down
                input.ki.dwFlags = 0; // Key down
            } else if (key.down == 1) { // Key up
                input.ki.dwFlags = KEYEVENTF_KEYUP; // Key up
            } else {
                continue; // No change, skip
            }

            #ifdef _DEBUG
            if (key.vk != 0 && key.down == 2) {
                throw std::runtime_error("Unexpected key state.");
            }
            #endif

            if (SendInput(1, &input, sizeof(INPUT)) == 0) {
                std::cerr << "INPUT: " << "SendInput failed with error: " << GetLastError() << std::endl;
                abort();
                return;
            }
        }


        auto now = std::chrono::steady_clock::now();

        if (now - lastprobe >= std::chrono::seconds(1)) {
            std::scoped_lock<std::mutex> lock(m_coutMutex);
            std::cout << "\r                                                       \r" << std::flush;
            std::cout << "INPUT: " << "Input frequency: " << count << " Hz | SGE: " << static_cast<int>(sgeCount) << std::flush;
            lastprobe = now;
            count = 0;
        }

    }
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