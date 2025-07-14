#include "NDSession.hpp"
#include "DesktopDuplication.hpp"
#include "D2DRenderer.hpp"
#include "D2DWindow.hpp"
#include <conio.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <wincodec.h>

constexpr char TEST_PORT[] = "54321";

#define RECV_CTXT ((void*)0x1000)
#define SEND_CTXT ((void*)0x2000)
#define READ_CTXT ((void*)0x3000)
#define WRITE_CTXT ((void*)0x4000)

#undef max
#undef min

constexpr size_t WIDTH = 2560;
constexpr size_t HEIGHT = 1440;
constexpr size_t FPS = 240;

constexpr size_t LENGTH_PER_FRAME = 1 + (WIDTH * HEIGHT * 4);
// Approx. 8.3MB per frame (FHD), consider batching. Send/Recv latency is around < 1 ms though.

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

uint64_t GetCurrentTimestamp() {
    return std::chrono::high_resolution_clock::now().time_since_epoch().count();
}

double CalculateGbps(uint64_t bytes, uint64_t nanoseconds) {
    return (static_cast<double>(bytes) * 8.0) / (static_cast<double>(nanoseconds) / 1e9) / 1e9;
}

double CalculateLatencyMicroseconds(uint64_t nanoseconds) {
    return static_cast<double>(nanoseconds) / 1000.0;
}

struct PeerInfo {
    UINT64 remoteAddr;
    UINT32 remoteToken;
};


void ShowUsage() {
    printf("main.exe [options]\n"
           "Options:\n"
           "\t-s <local_ip>           - Start as server\n"
           "\t-c <local_ip> <server_ip> - Start as client\n");
}

// MARK: TestServer
class TestServer : public NDSessionServerBase {
public:
    bool Setup(char* localAddr) {
        if (!Initialize(localAddr)) return false;

        ND2_ADAPTER_INFO info = GetAdapterInfo();
        if (info.AdapterId == 0) return false;

        std::cout << "Max transfer length: " << info.MaxTransferLength << std::endl;
        std::cout << "Max inline length: " << info.MaxInlineDataSize << std::endl;
        std::cout << "Max send sge: " << info.MaxInitiatorSge << std::endl;

        maxSge = info.MaxInitiatorSge;

        if (FAILED(CreateCQ(info.MaxCompletionQueueDepth))) return false;
        if (FAILED(CreateQP(info.MaxReceiveQueueDepth, info.MaxInitiatorQueueDepth, info.MaxReceiveSge, info.MaxInitiatorSge))) return false;
        if (FAILED(CreateMR())) return false;

        ULONG flags = ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE;
        if (FAILED(RegisterDataBuffer(LENGTH_PER_FRAME, flags))) return false;

        ND2_SGE sge = { 0 };
        sge.Buffer = m_Buf;
        sge.BufferLength = LENGTH_PER_FRAME;
        sge.MemoryRegionToken = m_pMr->GetLocalToken();

        if (FAILED(CreateListener())) return false;
        if (FAILED(CreateConnector())) return false;

        return true;
    }

    void Run(const char* localAddr) {
        // Initialize D2DRenderer
        HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(com_hr)) {
            std::cerr << "CoInitializeEx failed: " << std::hex << com_hr << std::endl;
            return;
        }

        D2DPresentation::D2DRenderer renderer;
        D2DPresentation::D2DWindow window("RDMA Texture Preview", WIDTH, HEIGHT);

        window.Start();

        HWND hwnd = NULL;
        while ((hwnd = window.GetHwnd()) == NULL) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        HRESULT hr = renderer.Initialize(nullptr, hwnd, WIDTH, HEIGHT, nullptr);
        if (FAILED(hr)) {
            std::cerr << "D2DRenderer initialization failed: " << std::hex << hr << std::endl;
            CoUninitialize();
            return;
        }

        char fullAddress[INET_ADDRSTRLEN + 6];
        sprintf_s(fullAddress, "%s:%s", localAddr, TEST_PORT);
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
        Bind(m_Buf, LENGTH_PER_FRAME, ND_OP_FLAG_ALLOW_WRITE | ND_OP_FLAG_ALLOW_READ);

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

        PeerInfo remoteInfo;
        remoteInfo.remoteAddr = receivedInfo->remoteAddr;
        remoteInfo.remoteToken = receivedInfo->remoteToken;

        memset(m_Buf, 0, LENGTH_PER_FRAME);
        
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

        memset(m_Buf, 0, LENGTH_PER_FRAME);

        // Flag test (0 = Nothing, 1 = Client has written, 2 = Server has read)
        // Reset to 0 after send completion
        // First bit of the memory is used as a flag
        unsigned char flag = 0;
        while (flag != 1) {
            flag = *static_cast<unsigned char*>(m_Buf);
            _mm_pause();
        }

        std::cout << "Flag is 1." << std::endl;

        memset(m_Buf, 2, 1);

        sge.Buffer = m_Buf;
        sge.BufferLength = 1;
        sge.MemoryRegionToken = m_pMr->GetLocalToken();

        if (FAILED(Write(&sge, 1, remoteInfo.remoteAddr, remoteInfo.remoteToken, 0, WRITE_CTXT))) {
            std::cerr << "Write failed." << std::endl;
            return;
        }
        if (!WaitForCompletionAndCheckContext(WRITE_CTXT)) {
            std::cerr << "WaitForCompletion for write failed." << std::endl;
            return;
        }

        ComPtr<ID3D11Device> d3dDevice = renderer.GetD3DDevice();
        ComPtr<ID3D11DeviceContext> d3dContext = renderer.GetD3DContext();

        std::cout << "Waiting for the frame..." << std::endl;

        int frames = 0;

        auto now = std::chrono::steady_clock::now();
        ID3D11Texture2D* frameTex = nullptr;

        flag = 0;

        auto lastDraw = std::chrono::steady_clock::time_point::max();
        std::vector<uint8_t> previousTex(LENGTH_PER_FRAME - 1, 0);

        while (true) {
            for (;;) {
                flag = *static_cast<unsigned char*>(m_Buf);
                if (flag == 1) {
                    std::atomic_thread_fence(std::memory_order_acquire);
                    goto newFrame;
                }
                if (std::chrono::steady_clock::now() - lastDraw > std::chrono::milliseconds(1000 / FPS)) goto noFrame;
                _mm_pause();
            }

            newFrame:
            {
                D3D11_TEXTURE2D_DESC desc;
                desc.Width = WIDTH;
                desc.Height = HEIGHT;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.SampleDesc.Quality = 0;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                desc.CPUAccessFlags = 0;
                desc.MiscFlags = 0;

                D3D11_SUBRESOURCE_DATA initData = {};
                initData.pSysMem = reinterpret_cast<uint8_t*>(m_Buf) + 1; // Skip the first byte which is the flag
                initData.SysMemPitch = desc.Width * 4;
                initData.SysMemSlicePitch = 0;

                std::copy(reinterpret_cast<uint8_t*>(m_Buf) + 1, 
                        reinterpret_cast<uint8_t*>(m_Buf) + LENGTH_PER_FRAME, 
                        previousTex.data());

                HRESULT hr = d3dDevice->CreateTexture2D(&desc, &initData, &frameTex);
                if (FAILED(hr)) {
                    std::cerr << "CreateTexture2D failed: " << std::hex << hr << std::endl;
                    throw new std::exception();
                }
            }

            goto draw;

            noFrame:
            {
                D3D11_TEXTURE2D_DESC desc;
                desc.Width = WIDTH;
                desc.Height = HEIGHT;
                desc.MipLevels = 1;
                desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.SampleDesc.Quality = 0;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
                desc.CPUAccessFlags = 0;
                desc.MiscFlags = 0;

                D3D11_SUBRESOURCE_DATA initData = {};
                initData.pSysMem = previousTex.data();
                initData.SysMemPitch = desc.Width * 4;
                initData.SysMemSlicePitch = 0;

                HRESULT hr = d3dDevice->CreateTexture2D(&desc, &initData, &frameTex);
                if (FAILED(hr)) {
                    std::cerr << "CreateTexture2D failed: " << std::hex << hr << std::endl;
                    throw new std::exception();
                }
            }

            draw:
            lastDraw = std::chrono::steady_clock::now();
            renderer.SetSourceSurface(frameTex);
            renderer.Render();

            frameTex->Release();

            //memset(m_Buf, 0, LENGTH_PER_FRAME);
            memset(m_Buf, 2, 1);

            if (_kbhit()) {
                char c = _getch();
                if (c == 'q' || c == 'Q') {
                    std::cout << "Exiting..." << std::endl;
                    break;
                }
            }

            auto now2 = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now2 - now).count();
            if (elapsed >= 1000000000) {
                std::cout << "\r                " << std::flush;
                std::cout << "FPS: " << frames << std::flush;
                frames = 0;
                now = now2;
            }

            frames++;
        }
        
        Shutdown();

        renderer.Cleanup();
        window.Stop();
        CoUninitialize();
    }
};

// MARK: TestClient
class TestClient : public NDSessionClientBase {
public:
    bool Setup(char* localAddr) {
        if (!Initialize(localAddr)) return false;

        ND2_ADAPTER_INFO info = GetAdapterInfo();
        if (info.AdapterId == 0) return false;

        if (FAILED(CreateCQ(info.MaxCompletionQueueDepth))) return false;
        if (FAILED(CreateQP(info.MaxReceiveQueueDepth, info.MaxInitiatorQueueDepth, info.MaxReceiveSge, info.MaxInitiatorSge))) return false;
        if (FAILED(CreateMR())) return false;

        ULONG flags = ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE;
        if (FAILED(RegisterDataBuffer(LENGTH_PER_FRAME, flags))) return false;
        if (FAILED(CreateConnector())) return false;

        return true;
    }

    void Run(const char* localAddr, const char* serverAddr) {
        ND2_SGE sge = { 0 };
        sge.Buffer = m_Buf;
        sge.BufferLength = LENGTH_PER_FRAME;
        sge.MemoryRegionToken = m_pMr->GetLocalToken();
        if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
            std::cerr << "PostReceive failed." << std::endl;
            return;
        }

        char fullServerAddress[INET_ADDRSTRLEN + 6];
        sprintf_s(fullServerAddress, "%s:%s", serverAddr, TEST_PORT);

        std::cout << "Connecting from " << localAddr << " to " << fullServerAddress << "..." << std::endl;
        if (FAILED(Connect(localAddr, fullServerAddress, 1, 1, nullptr, 0))) {
             std::cerr << "Connect failed." << std::endl;
             return;
        }

        if (FAILED(CompleteConnect())) {
            std::cerr << "CompleteConnect failed." << std::endl;
            return;
        }
        std::cout << "Connection established." << std::endl;

        CreateMW();
        Bind(m_Buf, LENGTH_PER_FRAME, ND_OP_FLAG_ALLOW_WRITE | ND_OP_FLAG_ALLOW_READ);

        PeerInfo* myInfo = reinterpret_cast<PeerInfo*>(m_Buf);
        myInfo->remoteAddr = reinterpret_cast<UINT64>(m_Buf);
        myInfo->remoteToken = m_pMw->GetRemoteToken();

        std::cout << "My address: " << myInfo->remoteAddr << ", token: " << myInfo->remoteToken << std::endl;

        std::this_thread::sleep_for(std::chrono::seconds(1));

        sge = { m_Buf, sizeof(PeerInfo), m_pMr->GetLocalToken() };
        if (FAILED(Send(&sge, 1, 0, SEND_CTXT))) {
            std::cerr << "Send failed." << std::endl;
            return;
        }
        if (!WaitForCompletionAndCheckContext(SEND_CTXT)) {
            std::cerr << "WaitForCompletion for send failed." << std::endl;
            return;
        }

        std::cout << "Peer information sent. Waiting for server's PeerInfo..." << std::endl;

        memset(m_Buf, 0, LENGTH_PER_FRAME);

        sge = { m_Buf, sizeof(PeerInfo), m_pMr->GetLocalToken() };

        if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
            std::cerr << "PostReceive for server's PeerInfo failed." << std::endl;
            return;
        }
        if (!WaitForCompletionAndCheckContext(RECV_CTXT)) {
            std::cerr << "WaitForCompletion for server's PeerInfo failed." << std::endl;
            return;
        }

        std::cout << "Received PeerInfo from server: remoteAddr = " 
                  << reinterpret_cast<PeerInfo*>(m_Buf)->remoteAddr
                  << ", remoteToken = " << reinterpret_cast<PeerInfo*>(m_Buf)->remoteToken << std::endl;

        PeerInfo remoteInfo;
        remoteInfo.remoteAddr = reinterpret_cast<PeerInfo*>(m_Buf)->remoteAddr;
        remoteInfo.remoteToken = reinterpret_cast<PeerInfo*>(m_Buf)->remoteToken;

        memset(m_Buf, 0, LENGTH_PER_FRAME);

        memset(m_Buf, 1, 1);
        sge.Buffer = m_Buf;
        sge.BufferLength = 1;
        sge.MemoryRegionToken = m_pMr->GetLocalToken();

        if (FAILED(Write(&sge, 1, remoteInfo.remoteAddr, remoteInfo.remoteToken, 0, WRITE_CTXT))) {
            std::cerr << "Write to server failed." << std::endl;
            return;
        }
        if (!WaitForCompletionAndCheckContext(WRITE_CTXT)) {
            std::cerr << "WaitForCompletion for write to server failed." << std::endl;
            return;
        }

        std::cout << "Write to server completed. Waiting for the flag..." << std::endl;
        unsigned char flag = 0;

        while (flag != 2) {
            flag = *static_cast<unsigned char*>(m_Buf);
            _mm_pause();
        }
        std::cout << "Flag is 2." << std::endl;

        memset(m_Buf, 0, LENGTH_PER_FRAME);

        std::cout << "Resetting the flag." << std::endl;
        if (FAILED(Write(&sge, 1, remoteInfo.remoteAddr, remoteInfo.remoteToken, 0, WRITE_CTXT))) {
            std::cerr << "Write to reset flag failed." << std::endl;
            return;
        }
        if (!WaitForCompletionAndCheckContext(WRITE_CTXT)) {
            std::cerr << "WaitForCompletion for reset flag write failed." << std::endl;
            return;
        }

        std::cout << "Sending a frame to the server." << std::endl;
        DesktopDuplication::Duplication& dupl = DesktopDuplication::Singleton<DesktopDuplication::Duplication>::Instance();

        while (true) {
            ID3D11Texture2D* frame;
            memset(m_Buf, 0, LENGTH_PER_FRAME);

            bool success = dupl.GetStagedTexture(frame, 1000 / FPS);
            if (!success) {
                //std::this_thread::sleep_for(std::chrono::milliseconds(1000/FPS/2)); // Timed out--probably no movement on the screen
                continue;
            }

            D3D11_TEXTURE2D_DESC desc;
            frame->GetDesc(&desc);
            uint8_t* frameData = reinterpret_cast<uint8_t*>(m_Buf);

            frameData[0] = 0;

            D3D11_MAPPED_SUBRESOURCE mappedResource;
            dupl.GetContext()->Map(frame, 0, D3D11_MAP_READ, 0 , &mappedResource);

            for (unsigned int i = 0; i < desc.Height; i++) {
                memcpy(frameData + 1 + i * desc.Width * 4, 
                    (uint8_t*)mappedResource.pData + i * mappedResource.RowPitch, 
                    desc.Width * 4);
            }

            dupl.GetContext()->Unmap(frame, 0);
            frame->Release();
            frame = nullptr;

            uint8_t flag = *reinterpret_cast<uint8_t*>(m_Buf);

            // Send the frame data
            sge.Buffer = m_Buf;
            sge.BufferLength = LENGTH_PER_FRAME;
            sge.MemoryRegionToken = m_pMr->GetLocalToken();

            if (FAILED(Write(&sge, 1, remoteInfo.remoteAddr, remoteInfo.remoteToken, 0, WRITE_CTXT))) {
                std::cerr << "Write of frame data failed." << std::endl;
                return;
            }
            if (!WaitForCompletionAndCheckContext(WRITE_CTXT)) {
                std::cerr << "WaitForCompletion for frame data write failed." << std::endl;
                return;
            }

            // Now set the flag
            reinterpret_cast<unsigned char*>(m_Buf)[0] = 1;
            sge.BufferLength = 1;
            if (FAILED(Write(&sge, 1, remoteInfo.remoteAddr, remoteInfo.remoteToken, 0, WRITE_CTXT))) {
                std::cerr << "Write to set flag failed." << std::endl;
                return;
            }
            if (!WaitForCompletionAndCheckContext(WRITE_CTXT)) {
                std::cerr << "WaitForCompletion for flag set failed." << std::endl;
                return;
            }

            auto lastSend = std::chrono::steady_clock::now();

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
            }

            auto now = std::chrono::steady_clock::now();
            auto dur = std::chrono::duration_cast<std::chrono::nanoseconds>(now - lastSend).count();

            while (dur < 1000000000 / FPS) {
                now = std::chrono::steady_clock::now();
                dur = std::chrono::duration_cast<std::chrono::nanoseconds>(now - lastSend).count();
            }
        }

        Shutdown();
    }
};

int main(int argc, char* argv[]) {
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

    if (!isServer) {
        DesktopDuplication::ChooseOutput();

        DesktopDuplication::Duplication& dupl = DesktopDuplication::Singleton<DesktopDuplication::Duplication>::Instance();
        dupl.InitDuplication();
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
        if (server.Setup(argv[2])) {
            server.Run(argv[2]);
        } else {
            std::cerr << "Server setup failed." << std::endl;
        }
    } else { // Client
        TestClient client;
        // Use the specific local IP for setup, and pass both to Run
        if (client.Setup(argv[2])) {
            client.Run(argv[2], argv[3]);
        } else {
            std::cerr << "Client setup failed." << std::endl;
        }
    }

    NdCleanup();
    WSACleanup();
    return 0;
}