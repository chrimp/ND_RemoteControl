#include "AudioNDSession.hpp"

#include <emmintrin.h>
#include <windows.h>
#include <vector>
#include <Functiondiscoverykeys_devpkey.h>

#undef min
#undef max

#include <algorithm>

#ifdef _DEBUG
#undef FAILED
    #define FAILED(hr) \
        ((hr) < 0 ? (std::cout << std::hex << hr, throw std::exception(), true) : false)
#endif

#pragma comment(lib, "synchronization.lib")

extern std::atomic<bool> g_shouldQuit;

constexpr char TEST_PORT[] = "54323";

void SetupAudioRenderer(_Out_ IAudioRenderClient*& pRenderClient, _Out_ IAudioClient*& pAudioClient, _In_ const HANDLE& hEvent) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* pEnum = nullptr;
    IMMDevice* pDevice = nullptr;

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&pEnum));
    if (FAILED(hr)) return;
    hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    if (FAILED(hr)) return;
    hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pAudioClient));
    if (FAILED(hr)) return;

    WAVEFORMATEX* pWaveFormat = nullptr;
    pAudioClient->GetMixFormat(&pWaveFormat);

    REFERENCE_TIME duration = 10 * 10000; // 10ms in 100-nanosecond units
    hr = pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
        duration, 0, pWaveFormat, nullptr);
    if (FAILED(hr)) return;
    hr = pAudioClient->SetEventHandle(hEvent);
    if (FAILED(hr)) return;
    hr = pAudioClient->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(&pRenderClient));
    if (FAILED(hr)) return;
    pAudioClient->AddRef();
    pRenderClient->AddRef();
}

bool AudioNDSessionServer::Setup(char* localAddr) {
    if (!Initialize(localAddr)) std::terminate();

    ND2_ADAPTER_INFO info = GetAdapterInfo();
    if (info.AdapterId == 0) std::terminate();

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
    
    std::cout << "AUDIO: Listening on " << fullAddress << std::endl;
    if (FAILED(Listen(fullAddress))) std::terminate();

    if (FAILED(GetConnectionRequest())) {
        std::cerr << "AUDIO: " << "GetConnectionRequest failed. Reason: " << std::hex << GetResult() << std::endl;
        return;
    }

    if (FAILED(Accept(1, 1, nullptr, 0))) std::terminate();
    std::cout << "AUDIO: Connection established." << std::endl;

    CreateMW();
    Bind(m_Buf, BUFFER_ALLOC_SIZE, ND_OP_FLAG_ALLOW_WRITE | ND_OP_FLAG_ALLOW_READ);
}

void AudioNDSessionServer::ExchangePeerInfo() {
    PeerInfo myInfo = { reinterpret_cast<UINT64>(m_Buf), m_pMw->GetRemoteToken() };

    ND2_SGE sge = {m_Buf, sizeof(PeerInfo), m_pMr->GetLocalToken()};
    if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
        std::cerr << "AUDIO: " << "PostReceive for PeerInfo failed." << std::endl;
        return;
    }

    if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
        std::cerr << "AUDIO: " << "WaitForCompletion for PeerInfo failed." << std::endl;
        return;
    }

    remoteInfo.remoteAddr = reinterpret_cast<PeerInfo*>(m_Buf)->remoteAddr;
    remoteInfo.remoteToken = reinterpret_cast<PeerInfo*>(m_Buf)->remoteToken;

    memset(m_Buf, 0, BUFFER_ALLOC_SIZE);
    memcpy(m_Buf, &myInfo, sizeof(PeerInfo));

    std::this_thread::sleep_for(std::chrono::seconds(1));

    if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
        std::cerr << "AUDIO: " << "Send for PeerInfo failed." << std::endl;
        return;
    }
    if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
        std::cerr << "AUDIO: " << "WaitForCompletion for PeerInfo send failed." << std::endl;
        return;
    }

    memset(m_Buf, 0, BUFFER_ALLOC_SIZE);
}

void AudioNDSessionServer::Loop() {
    uint8_t* flag = reinterpret_cast<uint8_t*>(m_Buf); // 0 = Cannot accomodate 1 = Good to go
    uint8_t* data = reinterpret_cast<uint8_t*>(m_Buf) + 1; // Audio data starts after the flag

    ND2_SGE flagSge = { m_Buf, 1, m_pMr->GetLocalToken() };
    ND2_SGE sge = { data, AUDIO_BUFFER_SIZE, m_pMr->GetLocalToken() };

    flag[0] = 0; // Reset flag
    _mm_clflush(m_Buf);
    _mm_sfence();

    auto flagWaitTotal = std::chrono::microseconds(0);
    auto lastCheck = std::chrono::steady_clock::now();

    unsigned int count = 0;

    IAudioRenderClient* pRenderClient = nullptr;
    IAudioClient* pAudioClient = nullptr;
    HANDLE hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    SetupAudioRenderer(pRenderClient, pAudioClient, hEvent);

    UINT32 bufferFrameCount;

    pAudioClient->GetBufferSize(&bufferFrameCount);
    if (bufferFrameCount != 480) { // 10ms
        std::cerr << "AUDIO: " << "Buffer size mismatch: " << bufferFrameCount << std::endl;
    }
    pAudioClient->Start();
    
    while (m_isRunning || !g_shouldQuit.load()) {
        WaitForSingleObject(hEvent, INFINITE);

        UINT32 padding = 0;
        UINT32 nBufferToWrite = bufferFrameCount;
        pAudioClient->GetCurrentPadding(&padding);
        nBufferToWrite -= padding;

        if (nBufferToWrite < SAMPLE_RATE / 1000 * 10) {
            std::cerr << "AUDIO: " << "Buffer is full, skipping frame." << std::endl;
            continue;
        }

        BYTE* pData = nullptr;
        HRESULT hr = pRenderClient->GetBuffer(nBufferToWrite, &pData);
        if (FAILED(hr)) {
            std::cerr << "AUDIO: " << "GetBuffer failed: " << std::hex << hr << std::endl;
            break;
        }
        if (pData == nullptr) {
            std::cerr << "AUDIO: " << "GetBuffer returned null data." << std::endl;
            abort();
            break;
        }

        // Get the data
        if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
            std::cerr << "AUDIO: " << "PostReceive for audio data failed." << std::endl;
            break;
        }
        flag[0] = 1;
        if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
            std::cerr << "AUDIO: " << "WaitForCompletion for audio data failed." << std::endl;
            break;
        }
        flag[0] = 0;

        memcpy(pData, data, AUDIO_BUFFER_SIZE);
        hr = pRenderClient->ReleaseBuffer(nBufferToWrite, 0);
        if (FAILED(hr)) {
            std::cerr << "AUDIO: " << "ReleaseBuffer failed: " << std::hex << hr << std::endl;
            break;
        }
    }
    pAudioClient->Stop();
    g_shouldQuit.store(true);
}

bool AudioNDSessionServer::WaitForCompletionAndCheckContext(void *expectedContext, ULONG notifyFlag) {
    ND2_RESULT ndRes = WaitForCompletion(notifyFlag, true);

    if (ND_SUCCESS != ndRes.Status) {
        std::cerr << "AUDIO: " << "Operation failed with status: " << std::hex << ndRes.Status << std::endl;
        #ifdef _DEBUG
        std::terminate();
        #endif
        return false;
    }
    if (expectedContext != ndRes.RequestContext) {
        std::cerr << "AUDIO: " << "Unexpected completion. Check for missing WaitForCompletion() call." << std::endl;
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

    std::this_thread::sleep_for(std::chrono::seconds(1));
    if (FAILED(Connect(localAddr, fullServerAddress, 1, 1, nullptr, 0))) {
        std::cerr << "AUDIO: " << "Connect failed." << std::endl;
        return;
    }
    if (FAILED(CompleteConnect())) {
        std::cerr << "AUDIO: " << "CompleteConnect failed." << std::endl;
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
        std::cerr << "AUDIO: " << "Send failed." << std::endl;
        return;
    }
    if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
        std::cerr << "AUDIO: " << "WaitForCompletion for PeerInfo send failed." << std::endl;
        return;
    }

    memset(m_Buf, 0, BUFFER_ALLOC_SIZE);
    sge = {m_Buf, sizeof(PeerInfo), m_pMr->GetLocalToken()};
    if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
        std::cerr << "AUDIO: " << "PostReceive for PeerInfo failed." << std::endl;
        return;
    }
    if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
        std::cerr << "AUDIO: " << "WaitForCompletion for PeerInfo receive failed." << std::endl;
        return;
    }

    remoteInfo.remoteAddr = reinterpret_cast<PeerInfo*>(m_Buf)->remoteAddr;
    remoteInfo.remoteToken = reinterpret_cast<PeerInfo*>(m_Buf)->remoteToken;

    memset(m_Buf, 0, BUFFER_ALLOC_SIZE);
}

void AudioNDSessionClient::Loop() {
    uint8_t* flag = reinterpret_cast<uint8_t*>(m_Buf); // 0 = Cannot accomodate 1 = Good to go
    uint8_t* data = reinterpret_cast<uint8_t*>(m_Buf) + 1; // Audio data starts after the flag
    ND2_SGE flagSge = { m_Buf, 1, m_pMr->GetLocalToken() };
    ND2_SGE sge = { data, AUDIO_BUFFER_SIZE, m_pMr->GetLocalToken() };

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* pEnum = nullptr;
    IMMDevice* pDevice = nullptr;
    IAudioClient* pAudioClient = nullptr;
    HANDLE hEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&pEnum));
    if (FAILED(hr)) {
        std::cerr << "AUDIO: " << "Failed to create IMMDeviceEnumerator instance." << std::hex << hr << std::endl;
        return;
    }
    hr = pEnum->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice);
    if (FAILED(hr)) {
        std::cerr << "AUDIO: " << "Failed to get default audio endpoint." << std::hex << hr << std::endl;
        pEnum->Release();
        return;
    }
    ComPtr<IPropertyStore> pProps;
    hr = pDevice->OpenPropertyStore(STGM_READ, pProps.GetAddressOf());
    if (FAILED(hr)) {
        std::cerr << "AUDIO: " << "Failed to open property store." << std::hex << hr << std::endl;
        pDevice->Release();
        pEnum->Release();
        return;
    }

    PROPVARIANT varName;
    PropVariantInit(&varName);
    hr = pProps->GetValue(PKEY_Device_FriendlyName, &varName);
    if (FAILED(hr)) {
        std::cerr << "AUDIO: " << "Failed to get device name." << std::hex << hr << std::endl;
        pDevice->Release();
        pEnum->Release();
        return;
    }

    _wsetlocale(LC_ALL, L"");
    std::wstring name = varName.pwszVal;
    PropVariantClear(&varName);
    std::wcout << L"AUDIO: " << L"Using audio device: " << name << std::endl;


    hr = pDevice->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(&pAudioClient));
    if (FAILED(hr)) {
        std::cerr << "AUDIO: " << "Failed to activate IAudioClient." << std::hex << hr << std::endl;
        pDevice->Release();
        pEnum->Release();
        return;
    }

    WAVEFORMATEX* pWaveFormat = nullptr;
    pAudioClient->GetMixFormat(&pWaveFormat);

    if (pWaveFormat->nSamplesPerSec != SAMPLE_RATE) {
        std::cerr << "AUDIO: " << "Sample rate mismatch: " << pWaveFormat->nSamplesPerSec << std::endl;
        std::cerr << "AUDIO: Using software resampling. Recommend using a device with " << SAMPLE_RATE << " Hz." << std::endl;
        return;
    }

    std::cout << "AUDIO: " << "Audio sample rate: " << pWaveFormat->nSamplesPerSec << std::endl;
    std::cout << "AUDIO: " << "Audio channels: " << pWaveFormat->nChannels << std::endl;
    std::cout << "AUDIO: " << "Audio bits per sample: " << pWaveFormat->wBitsPerSample << std::endl;
    std::cout << "AUDIO: " << "Audio format: " << pWaveFormat->wFormatTag << std::endl;

    pAudioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_LOOPBACK,
        0, 0, pWaveFormat, nullptr);
    pAudioClient->SetEventHandle(hEvent);

    hr = pAudioClient->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(m_audioCaptureClient.GetAddressOf()));
    if (FAILED(hr)) {
        std::cerr << "AUDIO: " << "Failed to get IAudioCaptureClient service." << std::hex << hr << std::endl;
        pAudioClient->Release();
        pDevice->Release();
        pEnum->Release();
        return;
    }

    std::this_thread::sleep_for(std::chrono::seconds(1)); // Give time for audio client to prepare
    
    pAudioClient->Start();

    while (m_isRunning || !g_shouldQuit.load()) {
        DWORD waitResult = WaitForSingleObject(hEvent, INFINITE);
        if (waitResult != WAIT_OBJECT_0) continue;

        UINT32 frames = 0;
        m_audioCaptureClient->GetNextPacketSize(&frames);
        if (frames == 0) continue;
        
        if (frames > AUDIO_BUFFER_SIZE) {
            std::cerr << "AUDIO: " << "Too many frames received: " << frames << std::endl;
            continue;
        }

        BYTE* buffer;
        DWORD flags = 0;

        HRESULT hr = m_audioCaptureClient->GetBuffer(&buffer, &frames, &flags, nullptr, nullptr);
        if (buffer == nullptr) {
            std::cerr << "AUDIO: " << "Audio buffer is null: " << std::hex << hr << std::endl;
            break;
        }

        flag[0] = 0;
        while (flag[0] != 1) {
            if (FAILED(Read(&flagSge, 1, remoteInfo.remoteAddr, remoteInfo.remoteToken, 0, READ_CTXT))) {
                std::cerr << "AUDIO: " << "Read failed." << std::endl;
                return;
            }
            if (!WaitForCompletionAndCheckContext(READ_CTXT, ND_CQ_NOTIFY_ANY)) {
                std::cerr << "AUDIO: " << "WaitForCompletion for flag read failed." << std::endl;
                return;
            }
        }

        // Send audio data
        memcpy(data, buffer, AUDIO_BUFFER_SIZE);
        if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
            std::cerr << "AUDIO: " << "Send failed." << std::endl;
            return;
        }
        if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
            std::cerr << "AUDIO: " << "WaitForCompletion for audio send failed." << std::endl;
            return;
        }

        memset(m_Buf, 0, BUFFER_ALLOC_SIZE);

        hr = m_audioCaptureClient->ReleaseBuffer(frames);
        if (FAILED(hr)) {
            std::cerr << "AUDIO: " << "ReleaseBuffer failed: " << std::hex << hr << std::endl;
            return;
        }
    }
    pAudioClient->Stop();
    g_shouldQuit.store(true);
}

bool AudioNDSessionClient::WaitForCompletionAndCheckContext(void *expectedContext, ULONG notifyFlag) {
    ND2_RESULT ndRes = WaitForCompletion(notifyFlag, true);

    if (ndRes.Status == ND_CANCELED) {
        std::cout << "AUDIO: " << "Remote has closed the connection." << std::endl;
        return false;
    }

    if (ND_SUCCESS != ndRes.Status) {
        std::cerr << "AUDIO: " << "Operation failed with status: " << std::hex << ndRes.Status << std::endl;
        #ifdef _DEBUG
        std::terminate();
        #endif
        return false;
    }
    if (expectedContext != ndRes.RequestContext) {
        std::cerr << "AUDIO: " << "Unexpected completion. Check for missing WaitForCompletion() call." << std::endl;
        throw std::exception();
        return false;
    }

    return true;
}