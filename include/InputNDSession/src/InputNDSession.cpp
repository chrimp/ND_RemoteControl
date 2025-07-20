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

void InputNDSessionServer::SendEvent(RAWINPUT input) {
    switch (input.header.dwType) {
        case RIM_TYPEMOUSE: {
            m_Mouse.x.fetch_add(static_cast<short>(input.data.mouse.lLastX));
            m_Mouse.y.fetch_add(static_cast<short>(input.data.mouse.lLastY));
            m_Mouse.wheel.fetch_add(input.data.mouse.usButtonData);
            m_Mouse.buttonFlags = input.data.mouse.ulButtons & ~ (RI_MOUSE_BUTTON_4_DOWN | RI_MOUSE_BUTTON_5_DOWN | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP | RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL);
            m_Mouse.buttonFlags = input.data.mouse.ulButtons * 2;
        }
    }
}

void InputNDSessionServer::Loop() {
    Point m_LastPos = {m_Mouse.x.load(), m_Mouse.y.load()};
    Move delta = {0, 0};
    MousePacket packet = {0, 0, 0, 0};

    ND2_SGE flagSge = { m_Buf, 1, m_pMr->GetLocalToken() };
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
        packet = { 
            m_Mouse.x.exchange(0), 
            m_Mouse.y.exchange(0), 
            m_Mouse.wheel.exchange(0), 
            m_Mouse.buttonFlags.load()
        };

        //if (delta.dx == 0 && delta.dy == 0) {
        //    continue; // No event to process
        //} Don't skip on zeros, now button state is also sent

        std::cout << "\r                                                                               \r" << std::flush;
        std::cout << "Move: " << packet.x << ", " << packet.y 
                  << " Wheel: " << static_cast<int>(packet.wheel) 
                  << " Flags: " << std::hex << packet.buttonFlags 
                  << std::dec << std::flush;

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
        ND2_SGE sge = { m_Buf, sizeof(MousePacket), m_pMr->GetLocalToken() };
        uint8_t* inputBuffer = reinterpret_cast<uint8_t*>(m_Buf) + 1;
        memcpy(inputBuffer, &packet, sizeof(MousePacket));
        if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
            std::cerr << "INPUT: " << "Send failed." << std::endl;
            return;
        }
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

    volatile uint8_t curSge = 0;
    uint16_t count = 0;
    //MousePacket received = {0, 0, 0, false, false, false};

    struct ShouldUP {
        bool leftB = false;
        bool rightB = false;
        bool middleB = false;
    };

    ShouldUP shouldUp = {false, false, false};

    ND2_SGE sge = { m_Buf, sizeof(MousePacket), m_pMr->GetLocalToken() };

    bool lb = false;
    bool rb = false;
    bool mb = false;

    if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
        std::cerr << "INPUT: " << "PostReceive failed." << std::endl;
        return;
    }
    curSge = 10;
    uint8_t* flag = reinterpret_cast<uint8_t*>(m_Buf); // 0 = Cannot accomodate 1 = Good to go
    uint8_t* buffer = reinterpret_cast<uint8_t*>(m_Buf) + 1; // Buffer for MousePacket

    while (m_isRunning) {
        if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
            std::cerr << "INPUT: " << "PostReceive failed." << std::endl;
            return;
        }

        // Wait 20 microseconds to see if it helps
        auto waitStart = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() - waitStart < std::chrono::microseconds(20)) {
            _mm_pause();
        }
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

        flag[0] = 0;
        _mm_clflush(m_Buf);
        _mm_sfence();

        /*
        int dx, dy;
        uint8_t* inputBuffer = reinterpret_cast<uint8_t*>(m_Buf) + 1;
        memcpy(&dx, inputBuffer, sizeof(dx));
        memcpy(&dy, inputBuffer + sizeof(dx), sizeof(dy));

        std::cout << "\r                                                                               \r" << std::flush;
        std::cout << "Move: " << dx << ", " << dy << std::flush;

        INPUT input = {0};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        input.mi.dx = dx;
        input.mi.dy = dy;

        if (SendInput(1, &input, sizeof(INPUT)) == 0) {
            std::cerr << "INPUT: " << "SendInput failed with error: " << GetLastError() << std::endl;
            abort();
            return;
        }
        */

        MousePacket* received = reinterpret_cast<MousePacket*>(buffer);
        INPUT input = {0};
        input.type = INPUT_MOUSE;
        input.mi.dx = received->x;
        input.mi.dy = received->y;
        //input.mi.mouseData = received->wheel;
        input.mi.dwFlags = MOUSEEVENTF_MOVE | received->buttonFlags;

        if (input.mi.dwFlags & MOUSEEVENTF_LEFTDOWN) lb = true;
        if (input.mi.dwFlags & MOUSEEVENTF_RIGHTDOWN) rb = true;
        if (input.mi.dwFlags & MOUSEEVENTF_MIDDLEDOWN) mb = true;
        if (input.mi.dwFlags & MOUSEEVENTF_LEFTUP) lb = false;
        if (input.mi.dwFlags & MOUSEEVENTF_RIGHTUP) rb = false;
        if (input.mi.dwFlags & MOUSEEVENTF_MIDDLEUP) mb = false;

        count++;

        if (SendInput(1, &input, sizeof(INPUT)) == 0) {
            std::cerr << "INPUT: " << "SendInput failed with error: " << GetLastError() << std::endl;
            abort();
            return;
        }
        /*
        std::cout << "\r                                                                               \r" << std::flush;
        std::cout << "Move: " << received->x << ", " << received->y << " LB: " << std::boolalpha << lb
                                                                    << " RB: " << std::boolalpha << rb 
                                                                    << " MB: " << std::boolalpha<< mb
                                                                    << " Flags: " << std::hex << input.mi.dwFlags << std::flush;
        */

        auto now = std::chrono::steady_clock::now();
        /*
        if (now - lastprobe >= std::chrono::seconds(1)) {
            std::scoped_lock<std::mutex> lock(m_coutMutex);
            std::cout << "\r                                                       \r" << std::flush;
            std::cout << "INPUT: " << "Input frequency: " << count << " Hz" << std::flush;
            lastprobe = now;
            count = 0;
        }
        */
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