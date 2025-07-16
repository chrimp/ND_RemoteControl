#include "InputNDSession.hpp"

#include <sstream>

#ifdef _DEBUG
#undef FAILED
    #define FAILED(hr) \
        ((hr) < 0 ? (throw std::runtime_error((std::ostringstream() << "Error: " << std::hex << hr).str()), true) : false)
#endif

constexpr char TEST_PORT[] = "54322";



bool InputNDSessionServer::Setup(char* localAddr) {
    if (!Initialize(localAddr)) std::terminate();

    ND2_ADAPTER_INFO info = GetAdapterInfo();
    if (info.AdapterId == 0) std::terminate();

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

void InputNDSessionServer::Loop() {
    ND2_SGE sge = { 0 };
    sge.Buffer = m_Buf;
    sge.BufferLength = INPUT_EVENT_BUFFER_SIZE;
    sge.MemoryRegionToken = m_pMr->GetLocalToken();

    constexpr unsigned short freq = 1000;
    constexpr unsigned short period = 1000 / freq;

    unsigned long long send = 0;

    while (m_isRunning) {
        auto thisIter = std::chrono::steady_clock::now();
        uint8_t* inputBuffer = reinterpret_cast<uint8_t*>(m_Buf);
        memcpy(inputBuffer, &send, sizeof(send));
        send++;

        if (FAILED(Write(&sge, 1, remoteInfo.remoteAddr, remoteInfo.remoteToken, 0, WRITE_CTXT))) {
            std::cerr << "INPUT: " << "Write failed." << std::endl;
            return;
        }
        if (!WaitForCompletionAndCheckContext(WRITE_CTXT)) {
            std::cerr << "INPUT: " << "WaitForCompletion for write failed." << std::endl;
            return;
        }

        auto end = std::chrono::steady_clock::now();
        while (end - thisIter < std::chrono::milliseconds(period)) {
            end = std::chrono::steady_clock::now();
            Sleep(0);
        }
        
        //_mm_pause();
        //std::this_thread::sleep_for(std::chrono::milliseconds(period));
    }
    Shutdown();
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
    ND2_SGE sge = {m_Buf, INPUT_EVENT_BUFFER_SIZE, m_pMr->GetLocalToken()};
    if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
        std::cerr << "INPUT: " << "PostReceive failed." << std::endl;
        return;
    }
    
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

    unsigned long long val = 0;
    unsigned long long newVal = 0;
    while (m_isRunning) {
        memcpy(&newVal, m_Buf, sizeof(val));

        auto now = std::chrono::steady_clock::now();

        if (now - lastprobe >= std::chrono::seconds(1)) {
            std::scoped_lock<std::mutex> lock(m_coutMutex);
            auto freq = newVal - val;
            std::cout << "\nINPUT: " << "Input frequency: " << freq << " Hz" << std::endl;
            lastprobe = now;
            val = newVal;
        }
        /*
        if (newVal - val >= 1000) {
            std::scoped_lock<std::mutex> lock(m_coutMutex);
            auto freq = newVal - val / std::chrono::duration_cast<std::chrono::seconds>(now - lastprobe).count();
            //std::cout << "INPUT: " << "\r                                                                  \r" << std::flush;
            std::cout << "\nINPUT: " << "Input frequency: " << freq << " Hz" << std::endl;
            lastprobe = now;
            val = newVal;
        }
        */
    }
}

bool InputNDSessionClient::WaitForCompletionAndCheckContext(void *expectedContext, ULONG notifyFlag) {
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
        return false;
    }

    return true;
}