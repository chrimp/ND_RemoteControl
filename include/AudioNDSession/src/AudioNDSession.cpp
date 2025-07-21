#include "AudioNDSession.hpp"

#include <emmintrin.h>
#include <windows.h>

#ifdef _DEBUG
#undef FAILED
    #define FAILED(hr) \
        ((hr) < 0 ? (std::cout << std::hex << hr, throw std::exception(), true) : false)
#endif

#pragma comment(lib, "synchronization.lib")

constexpr char TEST_PORT[] = "54323";


bool AudioNDSessionServer::Setup(char* localAddr) {
    if (!Initialize(localAddr)) std::terminate();

    ND2_ADAPTER_INFO info = GetAdapterInfo();
    if (info.AdapterId == 0) std::terminate();

    m_MaxSge = info.MaxInitiatorSge;

    if (FAILED(CreateCQ(info.MaxCompletionQueueDepth))) std::terminate();
    if (FAILED(CreateQP(info.MaxReceiveQueueDepth, info.MaxInitiatorQueueDepth, info.MaxReceiveSge, info.MaxInitiatorSge))) std::terminate();
    if (FAILED(CreateMR())) std::terminate();

    ULONG flags = ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE;
    if (FAILED(RegisterDataBuffer(BUFFER_ALLOC_SIZE, flags))) std::terminate();

    if (FAILED(CreateListener())) std::terminate();
    if (FAILED(CreateConnector())) std::terminate();

    return true;
}

void AudioNDSessionServer::OpenListener(const char* localAddr) {
    char fullAddress[INET_ADDRSTRLEN + 6];
    sprintf_s(fullAddress, "%s:%s", localAddr, TEST_PORT);
    
    if (FAILED(Listen(fullAddress))) std::terminate();

    if (FAILED(GetConnectionRequest())) {
        std::cerr << "INPUT: " << "GetConnectionRequest failed. Reason: " << std::hex << GetResult() << std::endl;
        return;
    }

    if (FAILED(Accept(1, 1, nullptr, 0))) std::terminate();

    CreateMW();
    Bind(m_Buf, BUFFER_ALLOC_SIZE, ND_OP_FLAG_ALLOW_WRITE | ND_OP_FLAG_ALLOW_READ);
}

void AudioNDSessionServer::ExchangePeerInfo() {
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

    memset(m_Buf, 0, BUFFER_ALLOC_SIZE);
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

    memset(m_Buf, 0, BUFFER_ALLOC_SIZE);
}

void AudioNDSessionServer::Loop() {
    ND2_SGE flagSge = { m_Buf, 1, m_pMr->GetLocalToken() };
    ND2_SGE sge = { m_Buf, sizeof(BUFFER_ALLOC_SIZE), m_pMr->GetLocalToken() }; // Do I NEED flag though? It's 10ms interval.
    volatile uint8_t* flag = reinterpret_cast<uint8_t*>(m_Buf);
    flag[0] = 0; // Reset flag
    _mm_clflush(m_Buf);
    _mm_sfence();

    auto flagWaitTotal = std::chrono::microseconds(0);
    auto lastCheck = std::chrono::steady_clock::now();

    unsigned int count = 0;

    while (m_isRunning) {
        flag[0] = 0; // Reset flag
        _mm_clflush(m_Buf);
        _mm_sfence();

        //WaitForSingleObject(m_hCallbackEvent, INFINITE);

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

bool AudioNDSessionServer::WaitForCompletionAndCheckContext(void *expectedContext, ULONG notifyFlag) {
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

bool AudioNDSessionClient::Setup(const char* localAddr) {
    if (!Initialize(const_cast<char*>(localAddr))) std::terminate();

    ND2_ADAPTER_INFO info = GetAdapterInfo();
    if (info.AdapterId == 0) std::terminate();

    if (FAILED(CreateCQ(info.MaxCompletionQueueDepth))) std::terminate();
    if (FAILED(CreateQP(info.MaxReceiveQueueDepth, info.MaxInitiatorQueueDepth, info.MaxReceiveSge, info.MaxInitiatorSge))) std::terminate();
    if (FAILED(CreateMR())) std::terminate();

    ULONG flags = ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE;
    if (FAILED(RegisterDataBuffer(BUFFER_ALLOC_SIZE, flags))) std::terminate();
    if (FAILED(CreateConnector())) std::terminate();

    return true;
}

void AudioNDSessionClient::OpenConnector(const char* localAddr, const char* serverAddr) {
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
    Bind(m_Buf, BUFFER_ALLOC_SIZE, ND_OP_FLAG_ALLOW_WRITE | ND_OP_FLAG_ALLOW_READ);
}

void AudioNDSessionClient::ExchangePeerInfo() {
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

    memset(m_Buf, 0, BUFFER_ALLOC_SIZE);
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

    memset(m_Buf, 0, BUFFER_ALLOC_SIZE);
}

void AudioNDSessionClient::Loop() {
    uint8_t* flag = reinterpret_cast<uint8_t*>(m_Buf); // 0 = Cannot accomodate 1 = Good to go
    //Packet* received = reinterpret_cast<Packet*>(reinterpret_cast<uint8_t*>(m_Buf) + 1); // Buffer for MousePacket

    while (m_isRunning) {
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
    }
}

bool AudioNDSessionClient::WaitForCompletionAndCheckContext(void *expectedContext, ULONG notifyFlag) {
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