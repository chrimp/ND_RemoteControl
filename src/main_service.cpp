#include "NDSession.hpp"
#include "DesktopDuplication.hpp"
#include "D2DRenderer.hpp"
#include "D2DWindow.hpp"
#include "InputNDSession.hpp"
#include "AudioNDSession.hpp"

#include <WtsApi32.h>
#include <conio.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <wincodec.h>
#include <mutex>
#include <Functiondiscoverykeys_devpkey.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#undef max
#undef min

std::atomic<bool> g_shouldQuit = false;

bool SaveFrameFromBuffer(const std::filesystem::path& path, ID3D11Texture2D* tex);

static size_t maxSge = -1;

std::string FormatBytes(uint64_t bytes) {
    if (bytes >= 1024ULL * 1024 * 1024) {
        return std::to_string(bytes / (1024ULL * 1024 * 1024)) + "GB";
    } else if (bytes >= 1024ULL * 1024) {
        return std::to_string(bytes / (1024ULL * 1024)) + "MB";
    } else if (bytes >= 1024ULL) {
        return std::to_string(bytes / 1024ULL) + "KB";
    } else {
        return std::to_string(bytes) + "B";
    }
}

void ShowUsage() {
    printf("main.exe [options]\n"
           "Options:\n"
           "\t-s <local_ip>           - Start as server\n"
           "\t-c <local_ip> <server_ip> - Start as client\n");
}

bool SyncThreadDesktop() {
    HDESK hDesk = OpenInputDesktop(DF_ALLOWOTHERACCOUNTHOOK, FALSE, GENERIC_ALL);
    if (hDesk == nullptr) {
        std::cerr << "OpenInputDesktop failed: " << GetLastError() << std::endl;
        return false;
    }

    if (!SetThreadDesktop(hDesk)) {
        std::cerr << "SetThreadDesktop failed: " << GetLastError() << std::endl;
        CloseDesktop(hDesk);
        return false;
    }

    CloseDesktop(hDesk);
    return true;
}

// MARK: TestServer
class TestServer : public NDSessionServerBase {
private:
    static void SendMultiCast(std::stop_token stopToken, const char* localAddr, const unsigned short port) {
        SOCKET mSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (mSock == INVALID_SOCKET) {
            std::cerr << "Failed to create multicast socket: " << WSAGetLastError() << std::endl;
            g_shouldQuit.store(true);
            return;
        }
        in_addr mCastIface = {};
        inet_pton(AF_INET, localAddr, &mCastIface);
        if (setsockopt(mSock, IPPROTO_IP, IP_MULTICAST_IF, reinterpret_cast<char*>(&mCastIface), sizeof(mCastIface)) == SOCKET_ERROR) {
            std::cerr << "Failed to set multicast interface: " << WSAGetLastError() << std::endl;
            closesocket(mSock);
            g_shouldQuit.store(true);
            return;
        }
        int ttl = 32;
        if (setsockopt(mSock, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<char*>(&ttl), sizeof(ttl)) == SOCKET_ERROR) {
            std::cerr << "Failed to set multicast TTL: " << WSAGetLastError() << std::endl;
            closesocket(mSock);
            g_shouldQuit.store(true);
            return;
        }

        sockaddr_in mAddr = {};
        mAddr.sin_family = AF_INET;
        inet_pton(AF_INET, "239.255.0.1", &mAddr.sin_addr);
        mAddr.sin_port = htons(56789);

        std::string message = std::string(localAddr) + ":" + std::to_string(port);

        while (!stopToken.stop_requested() && !g_shouldQuit.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            int res = sendto(mSock, message.c_str(), message.size(), 0, reinterpret_cast<sockaddr*>(&mAddr), sizeof(mAddr));
            if (res == SOCKET_ERROR) {
                std::cerr << "Failed to send multicast message: " << WSAGetLastError() << std::endl;
                g_shouldQuit.store(true);
                break;
            }
        }
    }

    static bool SetSockets(_In_ const char* localAddr, _Out_ SOCKET& listenSock, _Out_ unsigned short& port) {
        listenSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listenSock == INVALID_SOCKET) {
            std::cerr << "Failed to create listen socket: " << WSAGetLastError() << std::endl;
            g_shouldQuit.store(true);
            return false;
        }

        sockaddr_in localAddrin = {};
        localAddrin.sin_family = AF_INET;
        inet_pton(AF_INET, localAddr, &localAddrin.sin_addr);
        localAddrin.sin_port = htons(0);

        if (bind(listenSock, reinterpret_cast<sockaddr*>(&localAddrin), sizeof(localAddrin)) == SOCKET_ERROR) {
            std::cerr << "Failed to bind listen socket: " << WSAGetLastError() << std::endl;
            closesocket(listenSock);
            g_shouldQuit.store(true);
            return false;
        }

        sockaddr_in actual = {};
        int addrLen = sizeof(actual);
        if (getsockname(listenSock, reinterpret_cast<sockaddr*>(&actual), &addrLen) == SOCKET_ERROR) {
            std::cerr << "Failed to get socket name: " << WSAGetLastError() << std::endl;
            closesocket(listenSock);
            return false; 
        }
        port = ntohs(actual.sin_port);

        return true;
    }

    bool CreateTexutres() {
        ComPtr<ID3D11Device> d3dDevice = m_Renderer->GetD3DDevice();

        D3D11_TEXTURE2D_DESC yPlaneDesc = {};
        yPlaneDesc.Width = m_Width;
        yPlaneDesc.Height = m_Height;
        yPlaneDesc.MipLevels = 1;
        yPlaneDesc.ArraySize = 1;
        yPlaneDesc.Format = DXGI_FORMAT_R8_UNORM;
        yPlaneDesc.SampleDesc.Count = 1;
        yPlaneDesc.SampleDesc.Quality = 0;
        yPlaneDesc.Usage = D3D11_USAGE_DEFAULT;
        yPlaneDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        yPlaneDesc.CPUAccessFlags = 0;
        yPlaneDesc.MiscFlags = 0;

        HRESULT hr = d3dDevice->CreateTexture2D(&yPlaneDesc, nullptr, m_YPlaneTexture.GetAddressOf());
        if (FAILED(hr)) {
            std::cerr << "Failed to create Y plane texture: " << std::hex << hr << std::endl;
            return false;
        }

        D3D11_TEXTURE2D_DESC uvPlaneDesc = {};
        uvPlaneDesc.Width = m_Width;
        uvPlaneDesc.Height = m_Height / 2;
        uvPlaneDesc.MipLevels = 1;
        uvPlaneDesc.ArraySize = 1;
        uvPlaneDesc.Format = DXGI_FORMAT_R8G8_UNORM;
        uvPlaneDesc.SampleDesc.Count = 1;
        uvPlaneDesc.SampleDesc.Quality = 0;
        uvPlaneDesc.Usage = D3D11_USAGE_DEFAULT;
        uvPlaneDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        uvPlaneDesc.CPUAccessFlags = 0;
        uvPlaneDesc.MiscFlags = 0;

        hr = d3dDevice->CreateTexture2D(&uvPlaneDesc, nullptr, m_UVPlaneTexture.GetAddressOf());
        if (FAILED(hr)) {
            std::cerr << "Failed to create UV plane texture: " << std::hex << hr << std::endl;
            return false;
        }

        D3D11_TEXTURE2D_DESC outputDesc = {};
        outputDesc.Width = m_Width;
        outputDesc.Height = m_Height;
        outputDesc.MipLevels = 1;
        outputDesc.ArraySize = 1;
        outputDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        outputDesc.SampleDesc.Count = 1;
        outputDesc.SampleDesc.Quality = 0;
        outputDesc.Usage = D3D11_USAGE_DEFAULT;
        outputDesc.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        outputDesc.CPUAccessFlags = 0;
        outputDesc.MiscFlags = 0;

        hr = d3dDevice->CreateTexture2D(&outputDesc, nullptr, m_FrameTexture.GetAddressOf());
        if (FAILED(hr)) {
            std::cerr << "Failed to create output texture: " << std::hex << hr << std::endl;
            return false;
        }

        return true;
    }


public:
    bool Announce(char* localAddr) { // Multicast IP and Port
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "WSAStartup failed: " << WSAGetLastError() << std::endl;
            return false;
        }

        std::cout << "Starting Announce with localAddr: " << localAddr << std::endl;

        SOCKET listenSock, clientSock;
        unsigned short assignedPort = 0;

        if (!SetSockets(localAddr, listenSock, assignedPort)) {
            WSACleanup();
            return false;
        }

        m_listenPort = assignedPort;
        std::jthread multicastThread(SendMultiCast, localAddr, assignedPort);

        if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
            std::cerr << "Failed to listen on socket: " << WSAGetLastError() << std::endl;
            multicastThread.request_stop();
            closesocket(listenSock);
            return false; 
        }

        sockaddr_in clientAddr = {};
        int clientAddrLen = sizeof(clientAddr);

        clientSock = accept(listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLen);
        if (clientSock == INVALID_SOCKET) {
            std::cerr << "Failed to accept connection: " << WSAGetLastError() << std::endl;
            multicastThread.request_stop();
            closesocket(listenSock);
            return false;
        }

        multicastThread.request_stop();

        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, sizeof(clientIp));
        std::cout << "Client connected: " << clientIp << ":" << ntohs(clientAddr.sin_port) << std::endl;

        uint16_t buffer[3];
        int bytesReceived = recv(clientSock, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
        if (bytesReceived == SOCKET_ERROR) {
            std::cerr << "Failed to receive data: " << WSAGetLastError() << std::endl;
            closesocket(clientSock);
            closesocket(listenSock);
            return false;
        } else if (bytesReceived == 0) {
            std::cerr << "Connection closed by client." << std::endl;
            closesocket(clientSock);
            closesocket(listenSock);
            return false;
        }

        uint16_t width = buffer[0];
        uint16_t height = buffer[1];
        uint16_t refreshRate = buffer[2];

        std::cout << "Received resolution: " << width << "x" << height << " @ " << refreshRate << "Hz" << std::endl;

        m_Width = width;
        m_Height = height;
        m_RefreshRate = refreshRate;

        closesocket(clientSock);
        closesocket(listenSock);
        return true;
    }

    bool Setup(char* localAddr) {
        m_YPlaneSize = m_Width * m_Height;
        m_UVPlaneSize = m_Width * (m_Height / 2) * 2;
        m_LengthPerFrame = m_YPlaneSize + m_UVPlaneSize;
        m_BufferSize = 1 + m_LengthPerFrame;

        if (!Initialize(localAddr)) return false;

        ND2_ADAPTER_INFO info = GetAdapterInfo();
        if (info.AdapterId == 0) return false;

        std::cout << "Max transfer length: " << info.MaxTransferLength << std::endl;
        std::cout << "Max inline length: " << info.MaxInlineDataSize << std::endl;
        std::cout << "Max send sge: " << info.MaxInitiatorSge << std::endl;
        std::cout << "Max receive sge: " << info.MaxReceiveSge << std::endl;

        maxSge = info.MaxInitiatorSge;

        if (FAILED(CreateCQ(info.MaxCompletionQueueDepth))) return false;
        if (FAILED(CreateQP(info.MaxReceiveQueueDepth, info.MaxInitiatorQueueDepth, info.MaxReceiveSge, info.MaxInitiatorSge))) return false;
        if (FAILED(CreateMR())) return false;


        ULONG flags = ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE;
        if (FAILED(RegisterDataBuffer(m_BufferSize, flags))) return false;

        ND2_SGE sge = { 0 };
        sge.Buffer = m_Buf;
        sge.BufferLength = m_BufferSize;
        sge.MemoryRegionToken = m_pMr->GetLocalToken();

        if (FAILED(CreateListener())) return false;
        if (FAILED(CreateConnector())) return false;

        HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(com_hr)) {
            std::cerr << "CoInitializeEx failed: " << std::hex << com_hr << std::endl;
            return false;
        }

        m_Renderer = std::make_unique<D2DPresentation::D2DRenderer>();
        m_Window = std::make_unique<D2DPresentation::D2DWindow>("RDMA Texture Preview", m_Width, m_Height);

        m_Window->Start();

        while ((m_hWnd = m_Window->GetHwnd()) == NULL) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        HRESULT hr = m_Renderer->Initialize(nullptr, m_hWnd, m_Width, m_Height, nullptr);
        if (FAILED(hr)) {
            std::cerr << "D2DRenderer initialization failed: " << std::hex << hr << std::endl;
            CoUninitialize();
            return false;
        }

        m_Window->DisplayWindow();

        if (!CreateTexutres()) {
            std::cerr << "Failed to create textures." << std::endl;
            CoUninitialize();
            return false;
        }

        return true;
    }

    void OpenListener(const char* localAddr) {
        char fullAddress[INET_ADDRSTRLEN + 6];
        sprintf_s(fullAddress, "%s:%d", localAddr, m_listenPort);
        std::cout << "Listening on " << fullAddress << "..." << std::endl;
        if (FAILED(Listen(fullAddress))) return;

        std::cout << "Waiting for connection request..." << std::endl;
        if (FAILED(GetConnectionRequest())) {
            std::cout << "GetConnectionRequest failed. Reason: " << std::hex << GetResult() << std::endl;
            return;
        }

        std::cout << "Accepting connection..." << std::endl;
        if (FAILED(Accept(1, 1, nullptr, 0))) return;
        
        CreateMW();
        Bind(m_Buf, m_BufferSize, ND_OP_FLAG_ALLOW_WRITE | ND_OP_FLAG_ALLOW_READ);
    }

    void ExchangePeerInfo() {
        std::cout << "Connection established. Waiting for client's PeerInfo..." << std::endl;
        std::cout << "My address: " << reinterpret_cast<UINT64>(m_Buf) << ", token: " << m_pMw->GetRemoteToken() << std::endl;
        
        ND2_SGE sge = { 0 };
        sge.Buffer = m_Buf;
        sge.BufferLength = sizeof(PeerInfo);
        sge.MemoryRegionToken = m_pMr->GetLocalToken();

        if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
            std::cerr << "PostReceive for PeerInfo failed." << std::endl;
            return;
        }
        if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
            std::cerr << "WaitForCompletion for PeerInfo failed." << std::endl;
            return;
        }
        
        PeerInfo* receivedInfo = reinterpret_cast<PeerInfo*>(m_Buf);
        std::cout << "Received PeerInfo from client: remoteAddr = " << receivedInfo->remoteAddr
                  << ", remoteToken = " << receivedInfo->remoteToken << std::endl;

        sge = { m_Buf, sizeof(PeerInfo), m_pMr->GetLocalToken() };

        remoteInfo.remoteAddr = receivedInfo->remoteAddr;
        remoteInfo.remoteToken = receivedInfo->remoteToken;

        memset(m_Buf, 0, m_BufferSize);
        
        PeerInfo* myInfo = reinterpret_cast<PeerInfo*>(m_Buf);
        myInfo->remoteAddr = reinterpret_cast<UINT64>(m_Buf);
        myInfo->remoteToken = m_pMw->GetRemoteToken();

        sge.BufferLength = sizeof(PeerInfo);

        std::this_thread::sleep_for(std::chrono::seconds(1)); // Give time for client to prepare

        if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
            std::cerr << "Send of my PeerInfo failed." << std::endl;
            return;
        }
        if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
            std::cerr << "WaitForCompletion for my PeerInfo send failed." << std::endl;
            return;
        }

        memset(m_Buf, 0, m_BufferSize);
    }

    void Loop() {
        ComPtr<ID3D11Device> d3dDevice = m_Renderer->GetD3DDevice();
        ComPtr<ID3D11DeviceContext> d3dContext = m_Renderer->GetD3DContext();

        auto lastDraw = std::chrono::steady_clock::time_point::max();
        ND2_SGE sge = { 0 };

        auto FlagWaitTotal = std::chrono::microseconds(0);
        auto DecompressTotal = std::chrono::microseconds(0);
        auto DrawTotal = std::chrono::microseconds(0);

        bool isWindowOpen = true;

        while (isWindowOpen) {
            isWindowOpen = m_Window->isRunning();
            auto flagWaitStart = std::chrono::steady_clock::now();
            sge.Buffer = m_Buf;
            sge.BufferLength = 1;
            sge.MemoryRegionToken = m_pMr->GetLocalToken();
            if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
                std::cerr << "PostReceive for frame data failed." << std::endl;
                break;
            }

            *reinterpret_cast<uint8_t*>(m_Buf) = 2;

            if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
                std::cerr << "WaitForCompletion for frame data failed." << std::endl;
                break;
            }

            auto flagWaitEnd = std::chrono::steady_clock::now();
            FlagWaitTotal += std::chrono::duration_cast<std::chrono::microseconds>(flagWaitEnd - flagWaitStart);

            auto decompressStart = std::chrono::steady_clock::now();
            uint8_t flag = *static_cast<unsigned char*>(m_Buf);

            if (flag == 1) {
                std::atomic_thread_fence(std::memory_order_acquire);

                uint8_t* frameData = reinterpret_cast<uint8_t*>(m_Buf) + 1;

                d3dContext->UpdateSubresource(m_YPlaneTexture.Get(), 0, nullptr, frameData, m_Width, 0);
                d3dContext->UpdateSubresource(m_UVPlaneTexture.Get(), 0, nullptr, frameData + m_YPlaneSize, m_Width * 2, 0);

                m_Renderer->DecompressTexture(m_YPlaneTexture.Get(), m_UVPlaneTexture.Get(), m_FrameTexture.Get());

                m_Renderer->SetSourceSurface(m_FrameTexture.Get());

                D3D11_TEXTURE2D_DESC desc;
                m_FrameTexture->GetDesc(&desc);
            } else if (flag == 3) {
                continue;
            }
            auto decompressEnd = std::chrono::steady_clock::now();
            DecompressTotal += std::chrono::duration_cast<std::chrono::microseconds>(decompressEnd - decompressStart);

            lastDraw = std::chrono::steady_clock::now();
            
            auto drawStart = std::chrono::steady_clock::now();
            m_Renderer->Render();
            auto drawEnd = std::chrono::steady_clock::now();
            DrawTotal += std::chrono::duration_cast<std::chrono::microseconds>(drawEnd - drawStart);

            if (_kbhit()) {
                char c = _getch();
                if (c == 'q' || c == 'Q') {
                    std::cout << "Exiting..." << std::endl;
                    break;
                }
            }
        }
        
        Shutdown();

        g_shouldQuit.store(true);

        m_Renderer->Cleanup();
        m_Window->Stop();
        CoUninitialize();
    }

    void Run(const char* localAddr) {
        bool a = Announce(const_cast<char*>(localAddr));
        if (!a) return;
        inputSession.Start(const_cast<char*>(localAddr));
        audioSession.Start(const_cast<char*>(localAddr));
        Setup(const_cast<char*>(localAddr));
        m_Window->RegisterRawInputCallback([this](RAWINPUT rawInput) {
            inputSession.SendEvent(rawInput);
        }, inputSession.GetCallbackEvent());
        OpenListener(localAddr);
        ExchangePeerInfo();
        Loop();
        inputSession.Stop();
        audioSession.Stop();
    }

    private:
    std::unique_ptr<D2DPresentation::D2DRenderer> m_Renderer;
    std::unique_ptr<D2DPresentation::D2DWindow> m_Window;
    HWND m_hWnd = nullptr;

    ComPtr<ID3D11Texture2D> m_YPlaneTexture;
    ComPtr<ID3D11Texture2D> m_UVPlaneTexture;
    ComPtr<ID3D11Texture2D> m_FrameTexture;

    std::atomic<bool> m_isRunning = true;

    PeerInfo remoteInfo;

    InputNDSessionServer inputSession;
    AudioNDSessionServer audioSession;

    unsigned short m_Width = 0;
    unsigned short m_Height = 0;
    unsigned short m_RefreshRate = 0;

    unsigned short m_listenPort = 0;

    unsigned long m_LengthPerFrame = 0;
    unsigned long m_BufferSize = 0;

    unsigned long m_YPlaneSize = 0;
    unsigned long m_UVPlaneSize = 0;
};

// MARK: TestClient
class TestClient : public NDSessionClientBase {
private:
    bool LookForServer(char* localAddr, _Out_ std::string& serverAddr, _Out_ unsigned short& port) {
        std::cout << "Looking for server on multicast address" << std::endl;

        SOCKET mSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (mSock == INVALID_SOCKET) {
            std::cerr << "Failed to create socket: " << WSAGetLastError() << std::endl;
            return false;
        }

        sockaddr_in mAddr = {};
        mAddr.sin_family = AF_INET;
        inet_pton(AF_INET, localAddr, &mAddr.sin_addr);
        mAddr.sin_port = htons(56789);

        if (bind(mSock, reinterpret_cast<sockaddr*>(&mAddr), sizeof(mAddr)) == SOCKET_ERROR) {
            std::cerr << "Faield to bind socket: " << WSAGetLastError() << std::endl;
            closesocket(mSock);
            return false;
        }

        ip_mreq mreq = {};
        inet_pton(AF_INET, "239.255.0.1", &mreq.imr_multiaddr);
        inet_pton(AF_INET, localAddr, &mreq.imr_interface);

        if (setsockopt(mSock, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<char*>(&mreq), sizeof(mreq)) == SOCKET_ERROR) {
            std::cerr << "Failed to join multicast group: " << WSAGetLastError() << std::endl;
            closesocket(mSock);
            return false;
        }

        char buffer[128];
        sockaddr_in senderAddr = {};
        int senderAddrLen = sizeof(senderAddr);

        int bytesReceived = recvfrom(mSock, buffer, sizeof(buffer) - 1, 0, reinterpret_cast<sockaddr*>(&senderAddr), &senderAddrLen);
        if (bytesReceived == SOCKET_ERROR) {
            std::cerr << "Failed to receive data: " << WSAGetLastError() << std::endl;
            closesocket(mSock);
            return false;
        }

        buffer[bytesReceived] = '\0';
        std::string message(buffer);
        size_t colonPos = message.find_last_of(':');
        if (colonPos == std::string::npos) {
            std::cerr << "Invalid message format: " << message << std::endl;
            closesocket(mSock);
            return false;
        }

        std::string ip = message.substr(0, colonPos);
        std::string portStr = message.substr(colonPos + 1);
        port = static_cast<unsigned short>(std::stoi(portStr));
        serverAddr = ip;

        closesocket(mSock);
        return true;
    }

public:
    TestClient() : m_CoutMutex(), inputSession(m_CoutMutex) {}

    bool FindAndSendMode(char* localAddr) {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "WSAStartup failed: " << WSAGetLastError() << std::endl;
            return false;
        }

        SyncThreadDesktop();
        DesktopDuplication::ChooseOutput(m_Width, m_Height, m_RefreshRate);
        if (m_Width == 0 || m_Height == 0 || m_RefreshRate == 0) {
            throw std::runtime_error("Invalid output resolution or refresh rate.");
        }
        DesktopDuplication::Duplication& dupl = DesktopDuplication::Singleton<DesktopDuplication::Duplication>::Instance();
        if (!dupl.InitDuplication()) return false;

        std::string ip;
        unsigned short port;

        if (!LookForServer(localAddr, ip, port)) {
            std::cerr << "LookForServer failed." << std::endl;
            return false;
        }

        m_ServerPort = port;
        m_ServerAddress = ip;

        std::cout << "Found server at " << ip << ":" << port << std::endl;

        SOCKET tcpSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (tcpSock == INVALID_SOCKET) {
            std::cerr << "Failed to create TCP socket: " << WSAGetLastError() << std::endl;
            return false;
        }

        sockaddr_in serverAddr = {};
        serverAddr.sin_family = AF_INET;
        inet_pton(AF_INET, ip.c_str(), &serverAddr.sin_addr);
        serverAddr.sin_port = htons(port);

        int connectResult = connect(tcpSock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));
        if (connectResult == SOCKET_ERROR) {
            int error = WSAGetLastError();
            std::cerr << "Connect failed: " << error << std::endl;
            closesocket(tcpSock);
            return false;
        }

        uint16_t mode[3] = { m_Width, m_Height, m_RefreshRate };
        int bytesSent = send(tcpSock, reinterpret_cast<const char*>(mode), sizeof(mode), 0);
        if (bytesSent == SOCKET_ERROR) {
            std::cerr << "Failed to send mode: " << WSAGetLastError() << std::endl;
            closesocket(tcpSock);
            return false;
        }

        std::cout << "Sent mode: " << m_Width << "x" << m_Height << " @ " << m_RefreshRate << "Hz" << std::endl;

        closesocket(tcpSock);
        return true;
    }

    bool Setup(char* localAddr) {
        m_YPlaneSize = m_Width * m_Height;
        m_UVPlaneSize = m_Width * (m_Height / 2) * 2;
        m_LengthPerFrame = m_YPlaneSize + m_UVPlaneSize;
        m_BufferSize = 1 + m_LengthPerFrame;

        if (!Initialize(localAddr)) return false;

        ND2_ADAPTER_INFO info = GetAdapterInfo();
        if (info.AdapterId == 0) return false;

        if (FAILED(CreateCQ(info.MaxCompletionQueueDepth))) return false;
        if (FAILED(CreateQP(info.MaxReceiveQueueDepth, info.MaxInitiatorQueueDepth, info.MaxReceiveSge, info.MaxInitiatorSge))) return false;
        if (FAILED(CreateMR())) return false;

        ULONG flags = ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE;
        if (FAILED(RegisterDataBuffer(m_BufferSize, flags))) return false;
        if (FAILED(CreateConnector())) return false;

        return true;
    }

    bool OpenConnector(const char* localAddr) {
        const char* serverAddr = m_ServerAddress.c_str();

        ND2_SGE sge = { 0 };
        sge.Buffer = m_Buf;
        sge.BufferLength = m_BufferSize;
        sge.MemoryRegionToken = m_pMr->GetLocalToken();
        if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
            std::cerr << "PostReceive failed." << std::endl;
            return false;
        }

        char fullServerAddress[INET_ADDRSTRLEN + 6];
        sprintf_s(fullServerAddress, "%s:%d", serverAddr, m_ServerPort);

        std::cout << "Connecting from " << localAddr << " to " << fullServerAddress << "..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));

        if (FAILED(Connect(localAddr, fullServerAddress, 1, 1, nullptr, 0))) {
            std::cerr << "Connect failed." << std::endl;
            return false;
        }

        if (FAILED(CompleteConnect())) {
            std::cerr << "CompleteConnect failed." << std::endl;
            return false;
        }
        std::cout << "Connection established." << std::endl;

        CreateMW();
        Bind(m_Buf, m_BufferSize, ND_OP_FLAG_ALLOW_WRITE | ND_OP_FLAG_ALLOW_READ);

        return true;
    }

    bool ExchangePeerInfo() {
        PeerInfo* myInfo = reinterpret_cast<PeerInfo*>(m_Buf);
        myInfo->remoteAddr = reinterpret_cast<UINT64>(m_Buf);
        myInfo->remoteToken = m_pMw->GetRemoteToken();

        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        ND2_SGE sge = { 0 };
        sge.Buffer = m_Buf;
        sge.BufferLength = sizeof(PeerInfo);
        sge.MemoryRegionToken = m_pMr->GetLocalToken();

        sge = { m_Buf, sizeof(PeerInfo), m_pMr->GetLocalToken() };
        if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
            std::cerr << "Send failed." << std::endl;
            return false;
        }
        if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
            std::cerr << "WaitForCompletion for send failed." << std::endl;
            return false;
        }

        std::cout << "Peer information sent. Waiting for server's PeerInfo..." << std::endl;

        memset(m_Buf, 0, m_BufferSize);

        sge = { m_Buf, sizeof(PeerInfo), m_pMr->GetLocalToken() };

        if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
            std::cerr << "PostReceive for server's PeerInfo failed." << std::endl;
            return false;
        }
        if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
            std::cerr << "WaitForCompletion for server's PeerInfo failed." << std::endl;
            return false;
        }

        remoteInfo.remoteAddr = reinterpret_cast<PeerInfo*>(m_Buf)->remoteAddr;
        remoteInfo.remoteToken = reinterpret_cast<PeerInfo*>(m_Buf)->remoteToken;

        memset(m_Buf, 0, m_BufferSize);

        return true;
    }

    void Loop() {
        std::cout << "Sending frames to the server." << std::endl;
        DesktopDuplication::Duplication& dupl = DesktopDuplication::Singleton<DesktopDuplication::Duplication>::Instance();
        ND2_SGE sge = { 0 };

        auto GetAndCompressTotal = std::chrono::microseconds::zero();
        auto MapTotal = std::chrono::microseconds::zero();
        auto WriteTotal = std::chrono::microseconds::zero();
        auto FlagWaitTotal = std::chrono::microseconds::zero();

        while (true) {
            auto flagWaitStart = std::chrono::steady_clock::now();

            uint8_t flag = 0;
            // Wait for server to acknowledge the frame
            sge.Buffer = m_Buf;
            sge.BufferLength = 1;
            sge.MemoryRegionToken = m_pMr->GetLocalToken();

            while (flag != 2) {
                if (FAILED(Read(&sge, 1, remoteInfo.remoteAddr, remoteInfo.remoteToken, 0, READ_CTXT))) {
                    std::cerr << "Read failed." << std::endl;
                    return;
                }
                if (!WaitForCompletionAndCheckContext(READ_CTXT)) {
                    std::cerr << "WaitForCompletion for read failed." << std::endl;
                    return;
                }

                flag = *reinterpret_cast<uint8_t*>(m_Buf);
                _mm_pause();
            }
            auto flagWaitEnd = std::chrono::steady_clock::now();
            FlagWaitTotal += std::chrono::duration_cast<std::chrono::microseconds>(flagWaitEnd - flagWaitStart);


            auto GetAndCompressStart = std::chrono::steady_clock::now();
            ID3D11Texture2D* yPlane = nullptr;
            ID3D11Texture2D* uvPlane = nullptr;
            memset(m_Buf, 0, m_BufferSize);

            bool success = dupl.GetStagedTexture(yPlane, uvPlane, 1000 / m_RefreshRate);

            sge.Buffer = m_Buf;
            sge.BufferLength = 1;
            sge.MemoryRegionToken = m_pMr->GetLocalToken();

            if (!success) {
                *reinterpret_cast<uint8_t*>(m_Buf) = 3;
                if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
                    std::cerr << "Send of no frame failed." << std::endl;
                    return;
                }
                if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
                    std::cerr << "WaitForCompletion for no frame send failed." << std::endl;
                }
                continue;
            }
            auto GetAndCompressEnd = std::chrono::steady_clock::now();
            GetAndCompressTotal += std::chrono::duration_cast<std::chrono::microseconds>(GetAndCompressEnd - GetAndCompressStart);
            

            auto MapStart = std::chrono::steady_clock::now();
            uint8_t* frameData = reinterpret_cast<uint8_t*>(m_Buf);

            frameData[0] = 2;

            D3D11_MAPPED_SUBRESOURCE mappedResource;
            dupl.GetContext()->Map(yPlane, 0, D3D11_MAP_READ, 0 , &mappedResource);
            uint8_t* yDst = frameData + 1;
            uint8_t* ySrc = reinterpret_cast<uint8_t*>(mappedResource.pData);

            for (unsigned int i = 0; i < m_Height; i++) {
                memcpy(yDst, ySrc, m_Width);
                yDst += m_Width;
                ySrc += mappedResource.RowPitch;
            }

            dupl.GetContext()->Unmap(yPlane, 0);
            yPlane->Release();
            yPlane = nullptr;

            dupl.GetContext()->Map(uvPlane, 0, D3D11_MAP_READ, 0, &mappedResource);
            uint8_t* uvDst = frameData + 1 + m_YPlaneSize;
            uint8_t* uvSrc = reinterpret_cast<uint8_t*>(mappedResource.pData);

            for (unsigned int i = 0; i < m_Height / 2; i++) {
                memcpy(uvDst, uvSrc, m_Width * 2);
                uvDst += m_Width * 2;
                uvSrc += mappedResource.RowPitch;
            }
            dupl.GetContext()->Unmap(uvPlane, 0);
            uvPlane->Release();
            uvPlane = nullptr;
            auto MapEnd = std::chrono::steady_clock::now();
            MapTotal += std::chrono::duration_cast<std::chrono::microseconds>(MapEnd - MapStart);

            auto WriteStart = std::chrono::steady_clock::now();
            // Send the frame data
            sge.Buffer = m_Buf;
            sge.BufferLength = m_BufferSize;
            sge.MemoryRegionToken = m_pMr->GetLocalToken();

            if (FAILED(Write(&sge, 1, remoteInfo.remoteAddr, remoteInfo.remoteToken, 0, WRITE_CTXT))) {
                std::cerr << "Write of frame data failed." << std::endl;
                return;
            }
            if (!WaitForCompletionAndCheckContext(WRITE_CTXT)) {
                std::cerr << "WaitForCompletion for frame data write failed." << std::endl;
                return;
            }

            *reinterpret_cast<uint8_t*>(m_Buf) = 0; // Reset the flag

            sge.Buffer = m_Buf;
            sge.BufferLength = 1;
            sge.MemoryRegionToken = m_pMr->GetLocalToken();

            *reinterpret_cast<uint8_t*>(m_Buf) = 1;
            if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
                std::cerr << "Send of frame data failed." << std::endl;
                return;
            }
            if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
                std::cerr << "WaitForCompletion for frame data send failed." << std::endl;
                return;
            }
            auto WriteEnd = std::chrono::steady_clock::now();
            WriteTotal += std::chrono::duration_cast<std::chrono::microseconds>(WriteEnd - WriteStart);
        }

        Shutdown();

        g_shouldQuit.store(true);
    }

    void Run(const char* localAddr, const char* serverAddr) {
        FindAndSendMode(const_cast<char*>(localAddr));
        inputSession.Start(const_cast<char*>(localAddr), serverAddr);
        audioSession.Start(const_cast<char*>(localAddr), serverAddr);
        Setup(const_cast<char*>(localAddr));
        OpenConnector(localAddr);
        ExchangePeerInfo();
        Loop();
        inputSession.Stop();
        audioSession.Stop();
    }

    private:
    PeerInfo remoteInfo;
    std::string m_ServerAddress = "";
    unsigned short m_ServerPort = 0;

    std::atomic<bool> m_isRunning = true;
    std::mutex m_CoutMutex;

    InputNDSessionClient inputSession;
    AudioNDSessionClient audioSession;

    unsigned short m_Width = 0;
    unsigned short m_Height = 0;
    unsigned short m_RefreshRate = 0;

    unsigned long m_LengthPerFrame = 0;
    unsigned long m_BufferSize = 0;

    unsigned long m_YPlaneSize = 0;
    unsigned long m_UVPlaneSize = 0;
};

bool GetCurrentProcessUsername(std::wstring& username) {
    HANDLE hToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
        std::cerr << "Failed to open process token: " << GetLastError() << std::endl;
        return false;
    }

    DWORD size = 0;
    GetTokenInformation(hToken, TokenUser, nullptr, 0, &size);
    std::vector<BYTE> buffer(size);
    if (!GetTokenInformation(hToken, TokenUser, buffer.data(), size, &size)) {
        std::cerr << "Failed to get token information: " << GetLastError() << std::endl;
        CloseHandle(hToken);
        return false;
    }

    SID_NAME_USE sidType;
    wchar_t name[256], domain[256];
    DWORD nameSize = sizeof(name) / sizeof(wchar_t);
    DWORD domainSize = sizeof(domain) / sizeof(wchar_t);

    if (!LookupAccountSidW(nullptr, reinterpret_cast<PTOKEN_USER>(buffer.data())->User.Sid, name, &nameSize, domain, &domainSize, &sidType)) {
        std::cerr << "Failed to lookup account SID: " << GetLastError() << std::endl;
        CloseHandle(hToken);
        return false;
    }

    username = name;
    CloseHandle(hToken);
    return true;
}

bool GetActiveUserUsername(std::wstring& username) {
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) {
        std::cerr << "Failed to get active console session ID: " << GetLastError() << std::endl;
        return false;
    }

    LPWSTR userName = nullptr;
    DWORD bytesReturned = 0;
    if (WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, sessionId, WTSUserName, &userName, &bytesReturned) && userName) {
        username = userName;
        WTSFreeMemory(userName);
        return true;
    } else {
        std::cerr << "Failed to query username for session ID " << sessionId << ": " << GetLastError() << std::endl;
        return false;
    }
}

bool SpawnProcessInActiveSession(const wchar_t* processPath, int argc, char* argv[]) {
    DWORD sessionId = WTSGetActiveConsoleSessionId();
    if (sessionId == 0xFFFFFFFF) {
        std::cerr << "Failed to get active console session ID: " << GetLastError() << std::endl;
        return false;
    }

    HANDLE hToken = nullptr;
    if (!WTSQueryUserToken(sessionId, &hToken)) {
        std::cerr << "WTSQueryUserToken failed: " << GetLastError() << std::endl;
        return false;
    }

    HANDLE hDupToken = nullptr;
    if (!DuplicateTokenEx(hToken, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &hDupToken)) {
        std::cerr << "DuplicateTokenEx failed: " << GetLastError() << std::endl;
        CloseHandle(hToken);
        return false;
    }

    // Construct the command line with the arguments
    std::wstring commandLine = L"\"";
    commandLine += processPath;
    commandLine += L"\"";
    for (int i = 1; i < argc; ++i) {
        commandLine += L" ";
        std::wstring arg = std::wstring(argv[i], argv[i] + strlen(argv[i]));
        commandLine += L"\"";
        commandLine += arg;
        commandLine += L"\"";
    }

    STARTUPINFOW si = { sizeof(STARTUPINFOW) };
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessAsUserW(
            hDupToken,
            nullptr, // Use command line instead of application name
            commandLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            nullptr,
            &si,
            &pi)) {
        std::cerr << "CreateProcessAsUser failed: " << GetLastError() << std::endl;
        CloseHandle(hDupToken);
        CloseHandle(hToken);
        return false;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(hDupToken);
    CloseHandle(hToken);

    return true;
}
int main(int argc, char* argv[]) {
    #ifdef _DEBUG
    while (!IsDebuggerPresent()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    #endif
    if (argc < 3) {
        ShowUsage();
        return 1;
    }

    bool isServer = false;
    if (strcmp(argv[1], "-s") == 0) {
        if (argc != 3) { ShowUsage(); return 1; }
        isServer = true;
    } else if (strcmp(argv[1], "-c") == 0) {
        if (argc != 4) { ShowUsage(); return 1; }
        isServer = false;
    } else {
        ShowUsage();
        return 1;
    }

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed." << std::endl;
        return 1;
    }

    if (FAILED(NdStartup())) {
        std::cerr << "NdStartup failed." << std::endl;
        WSACleanup();
        return 1;
    }

    if (isServer) {
        TestServer server;
        server.Run(argv[2]);

    } else { // Client
        /*
        // Hardcoded path to the executable
        const wchar_t* processPath = L"C:\\Shares\\Inspur_ws\\main.exe";

        // Get the current process's username
        std::wstring currentUsername;
        if (!GetCurrentProcessUsername(currentUsername)) {
            return 1;
        }

        // Get the active user's username
        std::wstring activeUsername;
        if (!GetActiveUserUsername(activeUsername)) {
            return 1;
        }

        // Compare the usernames
        if (currentUsername != activeUsername) {
            std::wcout << L"Current user (" << currentUsername << L") does not match active user (" << activeUsername << L")." << std::endl;
            std::wcout << L"Spawning process in active user's session..." << std::endl;

            if (SpawnProcessInActiveSession(processPath, argc, argv)) {
                std::wcout << L"Process spawned successfully. Exiting current instance." << std::endl;
                return 0; // Exit the current instance
            } else {
                std::cerr << "Failed to spawn process in active user's session." << std::endl;
                return 1;
            }
        }

        std::wcout << L"Current user matches active user. Continuing execution..." << std::endl;
        */
        TestClient client;
        client.Run(argv[2], argv[3]);
    }

    NdCleanup();
    WSACleanup();
    return 0;
}

bool SaveFrameFromBuffer(const std::filesystem::path& path, ID3D11Texture2D* tex) {
    IWICImagingFactory* factory = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IWICStream* stream = nullptr;

    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);

    ID3D11Texture2D* stagingTexture = nullptr;
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    stagingDesc.MiscFlags = 0;

    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;
    tex->GetDevice(&d3dDevice);
    d3dDevice->GetImmediateContext(&d3dContext);

    std::vector<uint8_t> buffer(desc.Width * desc.Height * 4); // Assuming BGRA format

    d3dDevice->CreateTexture2D(&stagingDesc, nullptr, &stagingTexture);

    d3dContext->CopyResource(stagingTexture, tex);

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    HRESULT hr = d3dContext->Map(stagingTexture, 0, D3D11_MAP_READ, 0, &mappedResource);

    if (FAILED(hr)) throw std::exception();

    for (unsigned int i = 0; i < desc.Height; i++) {
        memcpy(buffer.data() + i * desc.Width * 4, 
               reinterpret_cast<uint8_t*>(mappedResource.pData) + i * mappedResource.RowPitch, 
               desc.Width * 4);
    }   

    unsigned int width = desc.Width;
    unsigned int height = desc.Height;

    hr = CoInitialize(nullptr);
    if (FAILED(hr)) return false;

    hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return false;

    hr = factory->CreateStream(&stream);
    if (FAILED(hr)) return false;

    std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm localTime;
    localtime_s(&localTime, &now);

    std::string time = std::format("{:04d}-{:02d}-{:02d}_{:02d}-{:02d}-{:02d}", localTime.tm_year + 1900, localTime.tm_mon + 1, localTime.tm_mday, localTime.tm_hour, localTime.tm_min, localTime.tm_sec);

    std::filesystem::path savePath = path / std::format("DeskDupl_{}.png", time);

    hr = stream->InitializeFromFilename(savePath.c_str(), GENERIC_WRITE);
    if (FAILED(hr)) return false;

    hr = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    if (FAILED(hr)) return false;

    hr = encoder->Initialize(stream, WICBitmapEncoderNoCache);
    if (FAILED(hr)) return false;

    hr = encoder->CreateNewFrame(&frame, nullptr);
    if (FAILED(hr)) return false;

    hr = frame->Initialize(nullptr);
    if (FAILED(hr)) return false;

    hr = frame->SetSize(width, height);
    if (FAILED(hr)) return false;

    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    hr = frame->SetPixelFormat(&format);
    if (FAILED(hr)) return false;

    hr = frame->WritePixels(height, width * 4, buffer.size(), buffer.data());
    if (FAILED(hr)) return false;

    frame->Commit();
    encoder->Commit();

    stream->Release();
    frame->Release();
    encoder->Release();
    factory->Release();

    CoUninitialize();

    return true;
}