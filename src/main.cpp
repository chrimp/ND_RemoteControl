#include "NDSession.hpp"
#include "DesktopDuplication.hpp"
#include "D2DRenderer.hpp"
#include "D2DWindow.hpp"
#include "InputNDSession.hpp"
#include "AudioNDSession.hpp"

#include <WinSock2.h>
#include <WtsApi32.h>
#include <conio.h>
#include <d3d11.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <wincodec.h>
#include <mutex>
#include <Functiondiscoverykeys_devpkey.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <future>

void SetupConsole() {
    if (!AllocConsole()) {
        std::string reason = "AllocConsole failed with error: " + std::to_string(GetLastError());
        throw std::runtime_error(reason);
    };
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
}

//#define DPRINT(x) printf("%s\n", x); fflush(stdout);
#define DPRINT(x)

//#define NOCONTROL
//#define NOAUDIO

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
    static void SendMultiCast(std::stop_token stopToken, const char* localAddr, const std::array<unsigned short, 3>& ports) {
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

        std::string port = std::to_string(ports[0]) + ";" + std::to_string(ports[1]) + ";" + std::to_string(ports[2]);

        std::string message = std::string(localAddr) + ":" + port;

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

    static bool SetSockets(_In_ const char* localAddr, _Out_ SOCKET& listenSock, _Out_ std::array<unsigned short, 3> & ports) {

        SOCKET listenSock1 = INVALID_SOCKET;
        SOCKET listenSock2 = INVALID_SOCKET;
        SOCKET listenSock3 = INVALID_SOCKET;

        auto bind_ephemeral = [&](SOCKET& sockOut, unsigned short& portOut) -> bool {
            sockOut = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (sockOut == INVALID_SOCKET) {
                std::cerr << "Failed to create socket: " << WSAGetLastError() << std::endl;
                g_shouldQuit.store(true);
                return false;
            }

            sockaddr_in addr = {};
            addr.sin_family = AF_INET;
            inet_pton(AF_INET, localAddr, &addr.sin_addr);
            addr.sin_port = htons(0); // request ephemeral port

            if (bind(sockOut, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
                std::cerr << "Failed to bind socket: " << WSAGetLastError() << std::endl;
                closesocket(sockOut);
                sockOut = INVALID_SOCKET;
                g_shouldQuit.store(true);
                return false;
            }

            sockaddr_in actual = {};
            int addrLen = sizeof(actual);
            if (getsockname(sockOut, reinterpret_cast<sockaddr*>(&actual), &addrLen) == SOCKET_ERROR) {
                std::cerr << "Failed to getsockname: " << WSAGetLastError() << std::endl;
                closesocket(sockOut);
                sockOut = INVALID_SOCKET;
                g_shouldQuit.store(true);
                return false;
            }

            portOut = ntohs(actual.sin_port);
            return true;
        };

        bool success = true;

        if (!bind_ephemeral(listenSock1, ports[0])) success = false;
        if (!bind_ephemeral(listenSock2, ports[1])) success = false;
        if (!bind_ephemeral(listenSock3, ports[2])) success = false;

        if (!success) {
            if (listenSock1 != INVALID_SOCKET) closesocket(listenSock1);
            if (listenSock2 != INVALID_SOCKET) closesocket(listenSock2);
            if (listenSock3 != INVALID_SOCKET) closesocket(listenSock3);
            return false;
        }
        
        listenSock = listenSock1;
        //closesocket(listenSock2);
        //closesocket(listenSock3);

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
        bool ret = false;

        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "WSAStartup failed: " << WSAGetLastError() << std::endl;
            return ret;
        }

        SOCKET listenSock, clientSock;

        //unsigned short assignedPort = 0;

        if (!SetSockets(localAddr, listenSock, m_listenPorts)) {
            WSACleanup();
            return ret;
        }

        //m_listenPorts = assignedPort;
        std::jthread multicastThread(SendMultiCast, localAddr, m_listenPorts);

        std::string ports = std::to_string(m_listenPorts[0]) + ", " + std::to_string(m_listenPorts[1]) + ", " + std::to_string(m_listenPorts[2]);

        std::cout << "Starting Announce with localAddr: " << localAddr << ", " << ports << std::endl;

        if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
            std::cerr << "Failed to listen on socket: " << WSAGetLastError() << std::endl;
            multicastThread.request_stop();
            closesocket(listenSock);
            return ret; 
        }

        sockaddr_in clientAddr = {};
        int clientAddrLen = sizeof(clientAddr);

        clientSock = accept(listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLen);
        if (clientSock == INVALID_SOCKET) {
            std::cerr << "Failed to accept connection: " << WSAGetLastError() << std::endl;
            multicastThread.request_stop();
            closesocket(listenSock);
            return ret;
        }

        multicastThread.request_stop();

        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, sizeof(clientIp));
        std::cout << "Client connected: " << clientIp << ":" << ntohs(clientAddr.sin_port) << std::endl;

        uint16_t buffer[4];
        int bytesReceived = recv(clientSock, reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
        if (bytesReceived == SOCKET_ERROR) {
            std::cerr << "Failed to receive data: " << WSAGetLastError() << std::endl;
            closesocket(clientSock);
            closesocket(listenSock);
            return ret;
        } else if (bytesReceived == 0) {
            std::cerr << "Connection closed by client." << std::endl;
            closesocket(clientSock);
            closesocket(listenSock);
            return ret;
        }

        uint16_t width = buffer[0];
        uint16_t height = buffer[1];
        uint16_t refreshRate = buffer[2];
        m_Compress = static_cast<bool>(buffer[3]);

        std::cout << "Received resolution: " << width << "x" << height << " @ " << refreshRate << "Hz" << " " << (m_Compress ? "Compressed" : "Raw") << std::endl;

        m_Width = width;
        m_Height = height;
        m_RefreshRate = refreshRate;

        closesocket(clientSock);
        closesocket(listenSock);
        return true;
    }

    bool Setup(char* localAddr) {
        if (m_Compress) {
            m_YPlaneSize = m_Width * m_Height;
            m_UVPlaneSize = m_Width * (m_Height / 2) * 2;
            m_LengthPerFrame = m_YPlaneSize + m_UVPlaneSize;
        } else {
            m_LengthPerFrame = m_Width * m_Height * 4;
        }
        m_BufferSize = 1 + m_LengthPerFrame;

        if (!Initialize(localAddr)) return false;

        ND2_ADAPTER_INFO info = GetAdapterInfo();
        if (info.AdapterId == 0) return false;

        std::cout << "Max transfer length: " << info.MaxTransferLength << std::endl;
        std::cout << "Max inline length: " << info.MaxInlineDataSize << std::endl;
        std::cout << "Max send sge: " << info.MaxInitiatorSge << std::endl;
        std::cout << "Max receive sge: " << info.MaxReceiveSge << std::endl;

        maxSge = info.MaxInitiatorSge;

        constexpr DWORD recvDepth = 4;
        constexpr DWORD initDepth = 4;

        HRESULT hr = CreateCQ(&m_pReceiveCq, recvDepth);
        if (FAILED(hr)) {
            std::cerr << "Failed to create receive completion queue: " << std::hex << hr << std::endl;
            return false;
        }

        hr = CreateCQ(&m_pInitiatorCq, initDepth);
        if (FAILED(hr)) {
            std::cerr << "Failed to create initiator completion queue: " << std::hex << hr << std::endl;
            return false;
        }
        if (FAILED(CreateQP(recvDepth, initDepth, 1, 1))) std::terminate();
        if (FAILED(CreateMR())) std::terminate();


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

        hr = m_Renderer->Initialize(nullptr, m_hWnd, m_Width, m_Height, nullptr);
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
        std::string fullAddress = std::string(localAddr) + ":" + std::to_string(m_listenPorts[0]);
        std::cout << "Listening on " << fullAddress << "..." << std::endl;
        if (FAILED(Listen(fullAddress.c_str()))) return;

        std::cout << "Waiting for connection request..." << std::endl;
        if (FAILED(GetConnectionRequest())) {
            std::cout << "GetConnectionRequest failed. Reason: " << std::hex << GetResult(m_pReceiveCq, &m_ReceiveOv) << std::endl;
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

    void CompressLoop() {
        unsigned int frames = 0;
        auto lastTime = std::chrono::steady_clock::now();

        ComPtr<ID3D11Device> d3dDevice = m_Renderer->GetD3DDevice();
        ComPtr<ID3D11DeviceContext> d3dContext = m_Renderer->GetD3DContext();

        auto lastDraw = std::chrono::steady_clock::time_point::max();
        ND2_SGE sge = { 0 };

        auto FlagWaitTotal = std::chrono::microseconds(0);
        auto DecompressTotal = std::chrono::microseconds(0);
        auto DrawTotal = std::chrono::microseconds(0);

        bool isWindowOpen = true;

        while (isWindowOpen && !g_shouldQuit.load()) {
            isWindowOpen = m_Window->isRunning();
            auto flagWaitStart = std::chrono::steady_clock::now();
            sge.Buffer = m_Buf;
            sge.BufferLength = 1;
            sge.MemoryRegionToken = m_pMr->GetLocalToken();
            if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
                std::cerr << "PostReceive for frame data failed." << std::endl;
                break;
            }
            DPRINT("PR for frame");

            *reinterpret_cast<uint8_t*>(m_Buf) = 2;

            if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
                std::cerr << "WaitForCompletion for frame data failed." << std::endl;
                break;
            }
            DPRINT("Completion for frame");

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
            } else {
                throw std::exception();
            }
            DPRINT("Decompressed frame");
            auto decompressEnd = std::chrono::steady_clock::now();
            DecompressTotal += std::chrono::duration_cast<std::chrono::microseconds>(decompressEnd - decompressStart);

            lastDraw = std::chrono::steady_clock::now();
            
            auto drawStart = std::chrono::steady_clock::now();
            m_Renderer->Render();
            auto drawEnd = std::chrono::steady_clock::now();
            DrawTotal += std::chrono::duration_cast<std::chrono::microseconds>(drawEnd - drawStart);

            frames++;
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastTime).count() >= 1) {
                std::cout << "\r                                                                                                       \r";
                std::cout << "FPS: " << frames << " | FlagWait: " << FlagWaitTotal.count() / frames
                          << "us | Decompress: " << DecompressTotal.count() / frames
                          << "us | Draw: " << DrawTotal.count() / frames << "us" << std::flush;
                frames = 0;
                FlagWaitTotal = std::chrono::microseconds(0);
                DecompressTotal = std::chrono::microseconds(0);
                DrawTotal = std::chrono::microseconds(0);
                lastTime = now;
            }

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

    void Loop() {
        unsigned int frames = 0;
        auto lastTime = std::chrono::steady_clock::now();

        ComPtr<ID3D11Device> d3dDevice = m_Renderer->GetD3DDevice();
        ComPtr<ID3D11DeviceContext> d3dContext = m_Renderer->GetD3DContext();

        auto lastDraw = std::chrono::steady_clock::time_point::max();
        ND2_SGE sge = { 0 };

        auto FlagWaitTotal = std::chrono::microseconds(0);
        auto DecompressTotal = std::chrono::microseconds(0);
        auto DrawTotal = std::chrono::microseconds(0);

        bool isWindowOpen = true;

        sge.Buffer = m_Buf;
        sge.BufferLength = 1;
        sge.MemoryRegionToken = m_pMr->GetLocalToken();
        if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
            std::cerr << "PostReceive for frame data failed." << std::endl;
            return;
        }

        while (isWindowOpen && !g_shouldQuit.load()) {
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
                d3dContext->UpdateSubresource(m_FrameTexture.Get(), 0, nullptr, frameData, m_Width * 4, 0);

                m_Renderer->SetSourceSurface(m_FrameTexture.Get());


                D3D11_TEXTURE2D_DESC desc;
                m_FrameTexture->GetDesc(&desc);
            } else {
                throw std::exception();
            }
            auto decompressEnd = std::chrono::steady_clock::now();
            DecompressTotal += std::chrono::duration_cast<std::chrono::microseconds>(decompressEnd - decompressStart);

            lastDraw = std::chrono::steady_clock::now();
            
            auto drawStart = std::chrono::steady_clock::now();
            m_Renderer->Render();
            auto drawEnd = std::chrono::steady_clock::now();
            DrawTotal += std::chrono::duration_cast<std::chrono::microseconds>(drawEnd - drawStart);

            
            frames++;
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - lastTime).count() >= 1) {
                std::cout << "\r                                                                                                                \r";
                std::cout << "FPS: " << frames << " | FlagWait: " << FlagWaitTotal.count() / frames
                          << "us | Decompress: " << DecompressTotal.count() / frames
                          << "us | Draw: " << DrawTotal.count() / frames << "us" << std::flush;
                frames = 0;
                FlagWaitTotal = std::chrono::microseconds(0);
                DecompressTotal = std::chrono::microseconds(0);
                DrawTotal = std::chrono::microseconds(0);
                lastTime = now;
            }
            
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

        #ifndef NOCONTROL
        inputSession.Start(const_cast<char*>(localAddr), m_listenPorts[1]);
        #endif

        #ifndef NOAUDIO
        audioSession.Start(const_cast<char*>(localAddr), m_listenPorts[2]);
        #endif

        if (!Setup(const_cast<char*>(localAddr))) return;
        m_Window->RegisterRawInputCallback([this](RAWINPUT rawInput) {
            inputSession.SendEvent(rawInput);
        }, inputSession.GetCallbackEvent());
        OpenListener(localAddr);
        ExchangePeerInfo();
        if (m_Compress) CompressLoop();
        else Loop();
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

    InputNDSession::InputNDSessionServer inputSession;
    AudioNDSession::AudioNDSessionServer audioSession;

    unsigned short m_Width = 0;
    unsigned short m_Height = 0;
    unsigned short m_RefreshRate = 0;
    bool m_Compress = false;

    std::array<unsigned short, 3> m_listenPorts = { 0 };

    unsigned long m_LengthPerFrame = 0;
    unsigned long m_BufferSize = 0;

    unsigned long m_YPlaneSize = 0;
    unsigned long m_UVPlaneSize = 0;
};

// MARK: TestClient
class TestClient : public NDSessionClientBase {
private:
    bool LookForServer(char* localAddr, _Out_ std::string& serverAddr, _Out_ std::array<unsigned short, 3>& port) {
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
            std::cerr << "Failed to bind socket: " << WSAGetLastError() << std::endl;
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
        std::string portsStr = message.substr(colonPos + 1);
        size_t firstSemicolon = portsStr.find(';');
        size_t secondSemicolon = portsStr.find(';', firstSemicolon + 1);
        if (firstSemicolon == std::string::npos || secondSemicolon == std::string::npos) {
            std::cerr << "Invalid ports format: " << portsStr << std::endl;
            closesocket(mSock);
            return false;
        }
        std::string portStr = portsStr.substr(0, firstSemicolon);
        port[0] = static_cast<unsigned short>(std::stoi(portStr));
        portStr = portsStr.substr(firstSemicolon + 1, secondSemicolon - firstSemicolon - 1);
        port[1] = static_cast<unsigned short>(std::stoi(portStr));
        portStr = portsStr.substr(secondSemicolon + 1);
        port[2] = static_cast<unsigned short>(std::stoi(portStr));

        serverAddr = ip;

        closesocket(mSock);
        return true;
    }

    bool AsyncWrite(uint8_t* data) {
        ND2_SGE sge = { 0 };
        uint8_t flag = 0;

        sge.Buffer = data;
        sge.BufferLength = 1;
        sge.MemoryRegionToken = m_pMr->GetLocalToken();

        std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();

        while (flag != 2) {
            if (FAILED(Read(&sge, 1, remoteInfo.remoteAddr, remoteInfo.remoteToken, 0, READ_CTXT))) {
                std::cerr << "Read failed." << std::endl;
                return false;
            }
            if (!WaitForCompletionAndCheckContext(READ_CTXT)) {
                std::cerr << "WaitForCompletion for read failed." << std::endl;
                return false;
            }

            /*auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::seconds>(now - start).count() >= 10) {
                std::cerr << "Read timeout." << std::endl;

                MessageBoxW(NULL, L"Read timeout. (AsyncWrite)" , L"Error", MB_OK | MB_ICONERROR);

                return false;
            }
            */

            flag = data[0];
            _mm_pause();
        }
        DPRINT("Read");
        sge.Buffer = data;
        sge.BufferLength = m_LengthPerFrame;
        sge.MemoryRegionToken = m_pMr->GetLocalToken();

        if (FAILED(Write(&sge, 1, remoteInfo.remoteAddr, remoteInfo.remoteToken, 0, WRITE_CTXT))) {
            std::cerr << "Write of frame data failed." << std::endl;
            return false;
        }
        if (!WaitForCompletionAndCheckContext(WRITE_CTXT)) {
            std::cerr << "WaitForCompletion for frame data write failed." << std::endl;
            return false;
        }
        DPRINT("Write");

        data[0] = 1;
        sge.Buffer = data;
        sge.BufferLength = 1;
        sge.MemoryRegionToken = m_pMr->GetLocalToken();

        if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
            std::cerr << "Send of frame data failed." << std::endl;
            return false;
        }
        if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
            std::cerr << "WaitForCompletion for frame data send failed." << std::endl;
            return false;
        }
        DPRINT("Send");

        return true;
    }

    void CreateTextures(bool compress) {
        DesktopDuplication::Duplication& dupl = DesktopDuplication::Singleton<DesktopDuplication::Duplication>::Instance();
        ComPtr<ID3D11Device> d3dDevice = dupl.GetDevice();
        std::array<HRESULT, 3> HResults;

        if (compress) {
            D3D11_TEXTURE2D_DESC yPlaneDesc = {};
            yPlaneDesc.Width = m_Width;
            yPlaneDesc.Height = m_Height;
            yPlaneDesc.MipLevels = 1;
            yPlaneDesc.ArraySize = 1;
            yPlaneDesc.Format = DXGI_FORMAT_R8_UNORM;
            yPlaneDesc.SampleDesc.Count = 1;
            yPlaneDesc.SampleDesc.Quality = 0;
            yPlaneDesc.Usage = D3D11_USAGE_STAGING;
            yPlaneDesc.BindFlags = 0;
            yPlaneDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            yPlaneDesc.MiscFlags = 0;

            D3D11_TEXTURE2D_DESC uvPlaneDesc = {};
            uvPlaneDesc.Width = m_Width;
            uvPlaneDesc.Height = m_Height / 2;
            uvPlaneDesc.MipLevels = 1;
            uvPlaneDesc.ArraySize = 1;
            uvPlaneDesc.Format = DXGI_FORMAT_R8G8_UNORM;
            uvPlaneDesc.SampleDesc.Count = 1;
            uvPlaneDesc.SampleDesc.Quality = 0;
            uvPlaneDesc.Usage = D3D11_USAGE_STAGING;
            uvPlaneDesc.BindFlags = 0;
            uvPlaneDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            uvPlaneDesc.MiscFlags = 0;

            HResults[0] = d3dDevice->CreateTexture2D(&yPlaneDesc, nullptr, m_YPlaneTexture.GetAddressOf());
            HResults[1] = d3dDevice->CreateTexture2D(&uvPlaneDesc, nullptr, m_UVPlaneTexture.GetAddressOf());
        }

        D3D11_TEXTURE2D_DESC frameDesc = {};
        frameDesc.Width = m_Width;
        frameDesc.Height = m_Height;
        frameDesc.MipLevels = 1;
        frameDesc.ArraySize = 1;
        frameDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        frameDesc.SampleDesc.Count = 1;
        frameDesc.SampleDesc.Quality = 0;
        frameDesc.Usage = D3D11_USAGE_STAGING;
        frameDesc.BindFlags = 0;
        frameDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        frameDesc.MiscFlags = 0;

        HResults[2] = d3dDevice->CreateTexture2D(&frameDesc, nullptr, m_FrameTexture.GetAddressOf());

        for (const auto& hr : HResults) {
            if (FAILED(hr)) {
                std::cerr << "Failed to create texture: " << std::hex << hr << std::endl;
                throw std::runtime_error("Texture creation failed.");
            }
        }

        return;
    }

public:
    TestClient() : m_CoutMutex(), inputSession(m_CoutMutex) {}

    bool FindAndSendMode(char* localAddr, bool compress) {
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "WSAStartup failed: " << WSAGetLastError() << std::endl;
            return false;
        }

        if (!m_isTest) {
            SyncThreadDesktop();
            DesktopDuplication::ChooseOutput(m_Width, m_Height, m_RefreshRate);
            if (m_Width == 0 || m_Height == 0 || m_RefreshRate == 0) {
                throw std::runtime_error("Invalid output resolution or refresh rate.");
            }
            DesktopDuplication::Duplication& dupl = DesktopDuplication::Singleton<DesktopDuplication::Duplication>::Instance();
            if (!dupl.InitDuplication()) return false;
        } else if (m_isTest) {
            m_Width = 1920;
            m_Height = 1080;
            m_RefreshRate = 360;
        }

        std::string ip;
        m_ServerPorts = { 0 };

        if (!LookForServer(localAddr, ip, m_ServerPorts)) {
            std::cerr << "LookForServer failed." << std::endl;
            return false;
        }

        //m_ServerPort = m_ServerPorts[0];
        m_ServerAddress = ip;

        std::cout << "Found server at " << ip << ":" << m_ServerPorts[0] << std::endl;

        SOCKET tcpSock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (tcpSock == INVALID_SOCKET) {
            std::cerr << "Failed to create TCP socket: " << WSAGetLastError() << std::endl;
            return false;
        }

        sockaddr_in serverAddr = {};
        serverAddr.sin_family = AF_INET;
        inet_pton(AF_INET, ip.c_str(), &serverAddr.sin_addr);
        serverAddr.sin_port = htons(m_ServerPorts[0]);

        int connectResult = connect(tcpSock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));
        if (connectResult == SOCKET_ERROR) {
            int error = WSAGetLastError();
            std::cerr << "Connect failed: " << error << std::endl;
            closesocket(tcpSock);
            return false;
        }

        uint16_t mode[4] = { m_Width, m_Height, m_RefreshRate, static_cast<uint16_t>(compress) };
        int bytesSent = send(tcpSock, reinterpret_cast<const char*>(mode), sizeof(mode), 0);
        if (bytesSent == SOCKET_ERROR) {
            std::cerr << "Failed to send mode: " << WSAGetLastError() << std::endl;
            closesocket(tcpSock);
            return false;
        }

        std::cout << "Sent mode: " << m_Width << "x" << m_Height << " @ " << m_RefreshRate << "Hz" << " " << (compress ? "Compressed" : "Uncompressed") << std::endl;

        closesocket(tcpSock);
        return true;
    }

    bool Setup(char* localAddr, bool compress) {
        if (compress) {
            m_YPlaneSize = m_Width * m_Height;
            m_UVPlaneSize = m_Width * (m_Height / 2) * 2;
            m_LengthPerFrame = m_YPlaneSize + m_UVPlaneSize;
        } else {
            m_LengthPerFrame = m_Width * m_Height * 4;
        }
        m_BufferSize = 1 + m_LengthPerFrame;
        m_BufferSize = m_BufferSize * 2;

        if (!m_isTest) CreateTextures(compress);

        if (!Initialize(localAddr)) return false;

        ND2_ADAPTER_INFO info = GetAdapterInfo();
        if (info.AdapterId == 0) return false;

        constexpr DWORD recvDepth = 4;
        constexpr DWORD initDepth = 4;

        HRESULT hr = CreateCQ(&m_pReceiveCq, recvDepth);
        if (FAILED(hr)) {
            std::cerr << "Failed to create receive completion queue: " << std::hex << hr << std::endl;
            return false;
        }

        hr = CreateCQ(&m_pInitiatorCq, initDepth);
        if (FAILED(hr)) {
            std::cerr << "Failed to create initiator completion queue: " << std::hex << hr << std::endl;
            return false;
        }
        if (FAILED(CreateQP(recvDepth, initDepth, 1, 1))) std::terminate();
        if (FAILED(CreateMR())) std::terminate();

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
        sprintf_s(fullServerAddress, "%s:%d", serverAddr, m_ServerPorts[0]);

        std::cout << "Connecting from " << localAddr << " to " << fullServerAddress << "..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(2));

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

    void CompressLoop() {
        std::cout << "Sending frames to the server." << std::endl;
        DesktopDuplication::Duplication& dupl = DesktopDuplication::Singleton<DesktopDuplication::Duplication>::Instance();
        ND2_SGE sge = { 0 };

        auto lastProbe = std::chrono::system_clock::now();
        auto GetAndCompressTotal = std::chrono::microseconds::zero();
        auto MapTotal = std::chrono::microseconds::zero();
        auto WriteTotal = std::chrono::microseconds::zero();
        auto FlagWaitTotal = std::chrono::microseconds::zero();
        int frames = 0;

        auto YMapTotal = std::chrono::microseconds::zero();
        auto YMemCpyTotal = std::chrono::microseconds::zero();
        auto UVMapTotal = std::chrono::microseconds::zero();
        auto UVMemCpyTotal = std::chrono::microseconds::zero();

        bool index = 0;
        uint8_t* buffers[] = { reinterpret_cast<uint8_t*>(m_Buf), reinterpret_cast<uint8_t*>(m_Buf) + (m_LengthPerFrame + 1) };

        std::future<bool> WriteFuture;

        ID3D11Texture2D* yPlane = m_YPlaneTexture.Get();
        ID3D11Texture2D* uvPlane = m_UVPlaneTexture.Get();

        while (!g_shouldQuit.load()) {
            index = !index;
            uint8_t* thisBuffer = buffers[index];

            auto GetAndCompressStart = std::chrono::steady_clock::now();

            bool success = dupl.GetStagedTexture(yPlane, uvPlane, 1000 / m_RefreshRate);
            DPRINT("Got frame");

            //sge.Buffer = thisBuffer;
            //sge.BufferLength = 1;
            //sge.MemoryRegionToken = m_pMr->GetLocalToken();

            if (!success) continue;

            auto GetAndCompressEnd = std::chrono::steady_clock::now();
            GetAndCompressTotal += std::chrono::duration_cast<std::chrono::microseconds>(GetAndCompressEnd - GetAndCompressStart);

            auto MapStart = std::chrono::steady_clock::now();

            thisBuffer[0] = 2;

            auto YMapStart = std::chrono::steady_clock::now();
            D3D11_MAPPED_SUBRESOURCE yMappedResource;
            D3D11_MAPPED_SUBRESOURCE uvMappedResource;
            dupl.GetContext()->Map(yPlane, 0, D3D11_MAP_READ, 0 , &yMappedResource);
            uint8_t* yDst = thisBuffer + 1;
            uint8_t* ySrc = reinterpret_cast<uint8_t*>(yMappedResource.pData);
            auto YMapEnd = std::chrono::steady_clock::now();
            YMapTotal += std::chrono::duration_cast<std::chrono::microseconds>(YMapEnd - YMapStart);

            auto YMemCpyStart = std::chrono::steady_clock::now();

            auto y_copy_future = std::async(std::launch::async, [=, this]() {
                const size_t yRowSize = static_cast<size_t>(m_Width);
                for (unsigned int i = 0; i < m_Height; i++) {
                    memcpy(yDst + i * yRowSize, ySrc + i * yMappedResource.RowPitch, yRowSize);
                }
            });
            /*
            for (unsigned int i = 0; i < m_Height; i++) {
                memcpy(yDst, ySrc, m_Width);
                yDst += m_Width;
                ySrc += mappedResource.RowPitch;
            }
            */

            //auto YMemCpyEnd = std::chrono::steady_clock::now();
            //YMemCpyTotal += std::chrono::duration_cast<std::chrono::microseconds>(YMemCpyEnd - YMemCpyStart);

            auto UVMapStart = std::chrono::steady_clock::now();
            dupl.GetContext()->Map(uvPlane, 0, D3D11_MAP_READ, 0, &uvMappedResource);
            uint8_t* uvDst = thisBuffer + 1 + m_YPlaneSize;
            uint8_t* uvSrc = reinterpret_cast<uint8_t*>(uvMappedResource.pData);
            auto UVMapEnd = std::chrono::steady_clock::now();
            UVMapTotal += std::chrono::duration_cast<std::chrono::microseconds>(UVMapEnd - UVMapStart);

            auto uv_copy_future = std::async(std::launch::async, [=, this]() {
                const size_t uvRowSize = static_cast<size_t>(m_Width * 2);
                for (unsigned int i = 0; i < m_Height / 2; i++) {
                    memcpy(uvDst + i * uvRowSize, uvSrc + i * uvMappedResource.RowPitch, uvRowSize);
                }
            });

            y_copy_future.get();
            uv_copy_future.get();
            auto YMemCpyEnd = std::chrono::steady_clock::now();
            YMemCpyTotal += std::chrono::duration_cast<std::chrono::microseconds>(YMemCpyEnd - YMemCpyStart);
            //auto UVMemCpyEnd = std::chrono::steady_clock::now();
            //UVMemCpyTotal += std::chrono::duration_cast<std::chrono::microseconds>(UVMemCpyEnd - UVMemCpyStart);

            dupl.GetContext()->Unmap(yPlane, 0);
            //yPlane->Release();
            //yPlane = nullptr;
            
            /*
            for (unsigned int i = 0; i < m_Height / 2; i++) {
                memcpy(uvDst, uvSrc, m_Width * 2);
                uvDst += m_Width * 2;
                uvSrc += mappedResource.RowPitch;
            }
            */
            dupl.GetContext()->Unmap(uvPlane, 0);
            //uvPlane->Release();
            //uvPlane = nullptr;
            auto MapEnd = std::chrono::steady_clock::now();
            MapTotal += std::chrono::duration_cast<std::chrono::microseconds>(MapEnd - MapStart);

            //UVMemCpyTotal += std::chrono::duration_cast<std::chrono::microseconds>(MapEnd - UVMemCpyStart);
            DPRINT("Map");

            auto WriteStart = std::chrono::steady_clock::now();

            if (WriteFuture.valid()) {
                if (!WriteFuture.get()) {
                    std::cerr << "AsyncWrite failed." << std::endl;
                    return;
                }
            }

            WriteFuture = std::async(std::launch::async, &TestClient::AsyncWrite, this, thisBuffer);
            auto WriteEnd = std::chrono::steady_clock::now();
            WriteTotal += std::chrono::duration_cast<std::chrono::microseconds>(WriteEnd - WriteStart);

            frames++;

            auto now = std::chrono::system_clock::now();
            if (false && std::chrono::duration_cast<std::chrono::seconds>(now - lastProbe).count() >= 1) {
                std::cout << "\r                                                                                                       \r";
                std::cout << "FPS: " << frames
                          << " | Get: " << GetAndCompressTotal.count() / frames
                          << "us | Map: " << MapTotal.count() / frames << "us"
                          << " | YMap: " << YMapTotal.count() / frames << "us"
                          << " | YMemCpy: " << YMemCpyTotal.count() / frames << "us"
                          << " | UVMap: " << UVMapTotal.count() / frames << "us"
                          << " | UVMemCpy: " << UVMemCpyTotal.count() / frames << "us"
                          << " | Write: " << WriteTotal.count() / frames << "us"
                          << std::flush;
                frames = 0;
                FlagWaitTotal = std::chrono::microseconds(0);
                GetAndCompressTotal = std::chrono::microseconds(0);
                MapTotal = std::chrono::microseconds(0);
                WriteTotal = std::chrono::microseconds(0);
                YMapTotal = std::chrono::microseconds(0);
                YMemCpyTotal = std::chrono::microseconds(0);
                UVMapTotal = std::chrono::microseconds(0);
                UVMemCpyTotal = std::chrono::microseconds(0);

                lastProbe = now;
            }            
        }

        Shutdown();

        g_shouldQuit.store(true);
    }

    void Loop() { // MARK: Remove flagging
        std::cout << "Sending frames to the server." << std::endl;
        ND2_SGE sge = { 0 };
        uint8_t* buffers[] = { reinterpret_cast<uint8_t*>(m_Buf), reinterpret_cast<uint8_t*>(m_Buf) + (m_LengthPerFrame + 1) };
        std::future<bool> WriteFuture;


        if (!m_isTest) {
            DesktopDuplication::Duplication& dupl = DesktopDuplication::Singleton<DesktopDuplication::Duplication>::Instance();
            
            auto lastProbe = std::chrono::system_clock::now();
            auto GetAndCompressTotal = std::chrono::microseconds::zero();
            auto MapTotal = std::chrono::microseconds::zero();
            auto WriteTotal = std::chrono::microseconds::zero();
            auto FlagWaitTotal = std::chrono::microseconds::zero();

            auto DevMapTotal = std::chrono::microseconds::zero();
            auto MemCpyTotal = std::chrono::microseconds::zero();
            int frames = 0;

            bool index = 0;

            ID3D11Texture2D* frameTexture = m_FrameTexture.Get();
            while (!g_shouldQuit.load()) {
                index = !index;
                uint8_t* thisBuffer = buffers[index];

                auto GetAndCompressStart = std::chrono::steady_clock::now();

                bool success = dupl.GetStagedTexture(frameTexture, 1000 / m_RefreshRate);
                DPRINT("GetTexture");

                sge.Buffer = thisBuffer;
                sge.BufferLength = 1;
                sge.MemoryRegionToken = m_pMr->GetLocalToken();
                
                if (!success) continue; // no need to notify

                auto GetAndCompressEnd = std::chrono::steady_clock::now();
                GetAndCompressTotal += std::chrono::duration_cast<std::chrono::microseconds>(GetAndCompressEnd - GetAndCompressStart);
                
                auto MapStart = std::chrono::steady_clock::now();

                thisBuffer[0] = 2;

                auto DevMapStart = std::chrono::steady_clock::now();
                D3D11_MAPPED_SUBRESOURCE mappedResource;
                dupl.GetContext()->Map(frameTexture, 0, D3D11_MAP_READ, 0 , &mappedResource);
                uint8_t* dst = thisBuffer + 1;
                uint8_t* src = reinterpret_cast<uint8_t*>(mappedResource.pData);
                auto DevMapEnd = std::chrono::steady_clock::now();
                DevMapTotal += std::chrono::duration_cast<std::chrono::microseconds>(DevMapEnd - DevMapStart);

                auto MemCpyStart = std::chrono::steady_clock::now();
                const size_t rowSize = static_cast<size_t>(m_Width * 4);

                const size_t avx_width = rowSize / 32;
                const size_t remainder_width = rowSize % 32;

                for (unsigned int i = 0; i < m_Height; ++i) {
                    uint8_t* p_dst_row = dst + i * rowSize;
                    uint8_t* p_src_row = src + i * mappedResource.RowPitch;
                    
                    // Process the bulk of the row in 32-byte chunks
                    for (size_t j = 0; j < avx_width; ++j) {
                        // Load 32 bytes from the source row
                        __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p_src_row));
                        // Store 32 bytes to the destination row
                        _mm256_storeu_si256(reinterpret_cast<__m256i*>(p_dst_row), chunk);
                        p_src_row += 32;
                        p_dst_row += 32;
                    }
                    
                    // Handle any remaining bytes that are not a multiple of 32
                    if (remainder_width > 0) {
                        memcpy(p_dst_row, p_src_row, remainder_width);
                    }
                }

                dupl.GetContext()->Unmap(frameTexture, 0);
                //frameTexture->Release();

                DPRINT("Map");
                auto MapEnd = std::chrono::steady_clock::now();
                MapTotal += std::chrono::duration_cast<std::chrono::microseconds>(MapEnd - MapStart);
                MemCpyTotal += std::chrono::duration_cast<std::chrono::microseconds>(MapEnd - MemCpyStart);

                auto WriteStart = std::chrono::steady_clock::now();
                // Start AsyncWrite()
                if (WriteFuture.valid()) {
                    if (!WriteFuture.get()) {
                        std::cerr << "AsyncWrite failed." << std::endl;
                        return;
                    }
                }
                DPRINT("Send");
                WriteFuture = std::async(std::launch::async, &TestClient::AsyncWrite, this, thisBuffer);
                auto WriteEnd = std::chrono::steady_clock::now();
                WriteTotal += std::chrono::duration_cast<std::chrono::microseconds>(WriteEnd - WriteStart);

                frames++;

                auto now = std::chrono::system_clock::now();
                if (false && std::chrono::duration_cast<std::chrono::seconds>(now - lastProbe).count() >= 1) {
                    std::cout << "\r                                                                                                                \r";
                    std::cout << "FPS: " << frames
                            << " | Get: " << GetAndCompressTotal.count() / frames
                            << "us | Map: " << MapTotal.count() / frames << "us"
                            << " | DevMap: " << DevMapTotal.count() / frames << "us"
                            << " | MemCpy: " << MemCpyTotal.count() / frames << "us"
                            << " | Write: " << WriteTotal.count() / frames << "us"
                            << std::flush;
                    frames = 0;
                    FlagWaitTotal = std::chrono::microseconds(0);
                    GetAndCompressTotal = std::chrono::microseconds(0);
                    MapTotal = std::chrono::microseconds(0);
                    WriteTotal = std::chrono::microseconds(0);

                    DevMapTotal = std::chrono::microseconds(0);
                    MemCpyTotal = std::chrono::microseconds(0);

                    lastProbe = now;
                }
            }
        } else if (m_isTest) {
            D3D_DRIVER_TYPE driverType = D3D_DRIVER_TYPE_WARP;
            UINT creationFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

            #ifdef _DEBUG
            creationFlags |= D3D11_CREATE_DEVICE_DEBUG;
            #endif

            ComPtr<ID3D11Device> d3dDevice;
            ComPtr<ID3D11DeviceContext> d3dContext;

            HRESULT hr = D3D11CreateDevice(
                nullptr,
                driverType,
                nullptr,
                creationFlags,
                nullptr,
                0,
                D3D11_SDK_VERSION,
                d3dDevice.GetAddressOf(),
                nullptr,
                d3dContext.GetAddressOf()
            );

            if (FAILED(hr)) {
                std::cerr << "Failed to create D3D11 device: " << std::hex << hr << std::endl;
                return;
            }

            ComPtr<IDXGIDevice> dxgiDevice;
            hr = d3dDevice->QueryInterface(IID_PPV_ARGS(dxgiDevice.GetAddressOf()));
            if (FAILED(hr)) {
                std::cerr << "Failed to query IDXGIDevice: " << std::hex << hr << std::endl;
                return;
            }

            ComPtr<ID2D1Factory1> d2dFactory;
            hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2dFactory.GetAddressOf());
            if (FAILED(hr)) {
                std::cerr << "Failed to create D2D1 factory: " << std::hex << hr << std::endl;
                return;
            }

            ComPtr<ID2D1Device> d2dDevice;
            hr = d2dFactory->CreateDevice(dxgiDevice.Get(), d2dDevice.GetAddressOf());
            if (FAILED(hr)) {
                std::cerr << "Failed to create D2D1 device: " << std::hex << hr << std::endl;
                return;
            }

            ComPtr<ID2D1DeviceContext> d2dContext;
            hr = d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2dContext.GetAddressOf());
            if (FAILED(hr)) {
                std::cerr << "Failed to create D2D1 device context: " << std::hex << hr << std::endl;
                return;
            }

            D2D1_SIZE_U size = D2D1::SizeU(m_Width, m_Height);

            D2D1_BITMAP_PROPERTIES1 bitmapProperties = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_TARGET,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
            );

            ComPtr<ID2D1Bitmap1> d2dBitmap;
            hr = d2dContext->CreateBitmap(size, nullptr, 0, &bitmapProperties, d2dBitmap.GetAddressOf());
            if (FAILED(hr)) {
                std::cerr << "Failed to create D2D1 bitmap: " << std::hex << hr << std::endl;
                return;
            }

            D2D1_BITMAP_PROPERTIES1 stagingBitmapProperties = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)
            );

            ComPtr<ID2D1Bitmap1> stagingBitmap;
            hr = d2dContext->CreateBitmap(size, nullptr, 0, &stagingBitmapProperties, stagingBitmap.GetAddressOf());

            ComPtr<ID2D1SolidColorBrush> brush;
            hr = d2dContext->CreateSolidColorBrush(D2D1::ColorF(0xF70063, 1.0f), &brush);
            if (FAILED(hr)) {
                std::cerr << "Failed to create D2D1 solid color brush: " << std::hex << hr << std::endl;
                return;
            }

            bool index = 0;

            short curposX = 0;
            short curposY = 0;

            while (!g_shouldQuit.load()) {
                index = !index;
                uint8_t* thisbuffer = buffers[index];

                thisbuffer[0] = 2;

                short x, y;
                inputSession.GetMouse(x, y);
                curposX += x;
                curposY += y;

                curposX = std::clamp(static_cast<int>(curposX), 0, static_cast<int>(m_Width - 1));
                curposY = std::clamp(static_cast<int>(curposY), 0, static_cast<int>(m_Height - 1));

                D2D1_ELLIPSE ellipse = D2D1::Ellipse(D2D1::Point2F(curposX, curposY), 30.0f, 30.0f);

                d2dContext->SetTarget(d2dBitmap.Get());
                d2dContext->BeginDraw();
                d2dContext->Clear(D2D1::ColorF(0x5882E8, 1.0f));
                d2dContext->FillEllipse(ellipse, brush.Get());

                d2dContext->EndDraw();

                stagingBitmap->CopyFromBitmap(nullptr, d2dBitmap.Get(), nullptr);

                D2D1_MAPPED_RECT mappedRect;
                stagingBitmap->Map(D2D1_MAP_OPTIONS_READ, &mappedRect);

                uint8_t* dst = thisbuffer + 1;
                uint8_t* src = mappedRect.bits;

                const size_t rowSize = static_cast<size_t>(m_Width * 4);

                const size_t avx_width = rowSize / 32;
                const size_t remainder_width = rowSize % 32;

                for (unsigned int i = 0; i < m_Height; ++i) {
                    uint8_t* p_dst_row = dst + i * rowSize;
                    uint8_t* p_src_row = src + i * mappedRect.pitch;

                    for (size_t j = 0; j < avx_width; ++j) {
                        __m256i chunk = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p_src_row));
                        _mm256_storeu_si256(reinterpret_cast<__m256i*>(p_dst_row), chunk);
                        p_src_row += 32;
                        p_dst_row += 32;
                    }

                    if (remainder_width > 0) {
                        memcpy(p_dst_row, p_src_row, remainder_width);
                    }
                }

                stagingBitmap->Unmap();

                if (WriteFuture.valid()) {
                    if (!WriteFuture.get()) {
                        std::cerr << "AsyncWrite failed." << std::endl;
                        return;
                    }
                }

                WriteFuture = std::async(std::launch::async, &TestClient::AsyncWrite, this, thisbuffer);
            }
        }

        Shutdown();

        g_shouldQuit.store(true);
    }

    void Run(const char* localAddr, bool compress) {
        FindAndSendMode(const_cast<char*>(localAddr), compress);
        if (m_isTest) {
            inputSession.EnableTestMode();
            inputSession.Start(const_cast<char*>(localAddr), m_ServerAddress.c_str(), m_ServerPorts[1]);
            audioSession.EnableTestMode();
            audioSession.Start(const_cast<char*>(localAddr), m_ServerAddress.c_str(), m_ServerPorts[2]);
            HANDLE hEvent;
            audioSession.GetEventHandle(hEvent);
            inputSession.SetEventHandle(hEvent);
        } else if (!m_isTest) {
            #ifndef NOCONTROL
            inputSession.Start(const_cast<char*>(localAddr), m_ServerAddress.c_str(), m_ServerPorts[1]);
            #endif
            #ifndef NOAUDIO
            audioSession.Start(const_cast<char*>(localAddr), m_ServerAddress.c_str(), m_ServerPorts[2]);
            #endif
        }

        Setup(const_cast<char*>(localAddr), compress);
        OpenConnector(localAddr);
        ExchangePeerInfo();
        if (compress) CompressLoop();
        else Loop();
        inputSession.Stop();
        audioSession.Stop();
    }

    void EnableTestMode() {
        m_isTest = true;
    }

    private:
    PeerInfo remoteInfo;
    std::string m_ServerAddress = "";
    std::array<unsigned short, 3> m_ServerPorts = { 0 };

    std::atomic<bool> m_isRunning = true;
    std::mutex m_CoutMutex;

    InputNDSession::InputNDSessionClient inputSession;
    AudioNDSession::AudioNDSessionClient audioSession;

    ComPtr<ID3D11Texture2D> m_YPlaneTexture;
    ComPtr<ID3D11Texture2D> m_UVPlaneTexture;
    ComPtr<ID3D11Texture2D> m_FrameTexture;

    unsigned short m_Width = 0;
    unsigned short m_Height = 0;
    unsigned short m_RefreshRate = 0;

    unsigned long m_LengthPerFrame = 0;
    unsigned long m_BufferSize = 0;

    unsigned long m_YPlaneSize = 0;
    unsigned long m_UVPlaneSize = 0;

    bool m_isTest = false;
};

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
    bool isTest = false;

    if (strcmp(argv[1], "-s") == 0) {
        //if (argc == 4) {
            //if (strcmp(argv[3], "-T") == 0) {
            //    isTest = true;
            //}
        //}
        //else if (argc != 3) { ShowUsage(); return 1; }
        if (argc != 3) { ShowUsage(); return 1; }
        isServer = true;
    } else if (strcmp(argv[1], "-c") == 0) {
        if (argc == 5) {
            if (strcmp(argv[4], "-T") == 0) {
                isTest = true;
            }
        } else if (argc != 4) { ShowUsage(); return 1; }
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
    } else {
        TestClient client;
        bool compress = false;

        if (_stricmp(argv[3], "r") == 0) {
            if (isTest) client.EnableTestMode();
            compress = false;
        } else if (_stricmp(argv[3], "c") == 0) {
            if (isTest) {
                std::cerr << "Test mode is not supported with compression." << std::endl;
                return 1;
            }
            compress = true;
        } else {
            std::cerr << "Invalid compression flag. Use 'r' for raw or 'c' for compressed." << std::endl;
            return 1;
        }

        client.Run(argv[2], compress);
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