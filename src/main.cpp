#include "NDSession.hpp"
#include "DesktopDuplication.hpp"
#include "D2DRenderer.hpp"
#include "D2DWindow.hpp"
#include "InputNDSession.hpp"
#include <conio.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <wincodec.h>
#include <mutex>

constexpr char TEST_PORT[] = "54321";

#undef max
#undef min

constexpr size_t WIDTH = 1920;
constexpr size_t HEIGHT = 1080;
constexpr size_t FPS = 120;

constexpr size_t Y_PLANE_SIZE = WIDTH * HEIGHT;
constexpr size_t UV_PLANE_SIZE = WIDTH * (HEIGHT / 2) * 2;

constexpr size_t LENGTH_PER_FRAME = 1 + Y_PLANE_SIZE + UV_PLANE_SIZE;

bool SaveFrameFromBuffer(const std::filesystem::path& path, ID3D11Texture2D* tex);

enum class InputType : uint8_t {
    Key = 0,
    MouseMove = 1,
    MouseButton = 2
};

#pragma pack(push, 1)
struct InputEvent {
    InputType type;
    union {
        struct { uint16_t vk; bool down; } key;
        struct { float dx; float dy; } mouseMove;
        struct { uint8_t button; bool down; } mouseButton;
    };
};
#pragma pack(pop)

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
        std::cout << "Max receive sge: " << info.MaxReceiveSge << std::endl;

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

        HRESULT com_hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        if (FAILED(com_hr)) {
            std::cerr << "CoInitializeEx failed: " << std::hex << com_hr << std::endl;
            return false;
        }

        m_Renderer = std::make_unique<D2DPresentation::D2DRenderer>();
        m_Window = std::make_unique<D2DPresentation::D2DWindow>("RDMA Texture Preview", WIDTH, HEIGHT);

        m_Window->Start();

        while ((m_hWnd = m_Window->GetHwnd()) == NULL) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        HRESULT hr = m_Renderer->Initialize(nullptr, m_hWnd, WIDTH, HEIGHT, nullptr);
        if (FAILED(hr)) {
            std::cerr << "D2DRenderer initialization failed: " << std::hex << hr << std::endl;
            CoUninitialize();
            return false;
        }

        m_Window->DisplayWindow();
        PostMessage(m_hWnd, WM_SYSCOMMAND, SC_MINIMIZE, 0);

        ComPtr<ID3D11Device> d3dDevice = m_Renderer->GetD3DDevice();
        ComPtr<ID3D11DeviceContext> d3dContext = m_Renderer->GetD3DContext();

        D3D11_TEXTURE2D_DESC yPlaneDesc = {};
        yPlaneDesc.Width = WIDTH;
        yPlaneDesc.Height = HEIGHT;
        yPlaneDesc.MipLevels = 1;
        yPlaneDesc.ArraySize = 1;
        yPlaneDesc.Format = DXGI_FORMAT_R8_UNORM; // Single channel for Y
        yPlaneDesc.SampleDesc.Count = 1;
        yPlaneDesc.SampleDesc.Quality = 0;
        yPlaneDesc.Usage = D3D11_USAGE_DEFAULT;
        yPlaneDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        yPlaneDesc.CPUAccessFlags = 0;
        yPlaneDesc.MiscFlags = 0;

        hr = d3dDevice->CreateTexture2D(&yPlaneDesc, nullptr, m_YPlaneTexture.GetAddressOf());
        if (FAILED(hr)) {
            std::cerr << "Failed to create Y plane texture: " << std::hex << hr << std::endl;
            return false;
        }

        D3D11_TEXTURE2D_DESC uvPlaneDesc = {};
        uvPlaneDesc.Width = WIDTH;
        uvPlaneDesc.Height = HEIGHT / 2; // 4:4:0 subsampling
        uvPlaneDesc.MipLevels = 1;
        uvPlaneDesc.ArraySize = 1;
        uvPlaneDesc.Format = DXGI_FORMAT_R8G8_UNORM; // Two channels for UV
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

        D3D11_TEXTURE2D_DESC yDesc;
        m_YPlaneTexture->GetDesc(&yDesc);
        D3D11_TEXTURE2D_DESC uvDesc;
        m_UVPlaneTexture->GetDesc(&uvDesc);

        D3D11_TEXTURE2D_DESC frameDesc = {};
        frameDesc.Width = WIDTH;
        frameDesc.Height = HEIGHT;
        frameDesc.MipLevels = 1;
        frameDesc.ArraySize = 1;
        frameDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // BGRA format
        frameDesc.SampleDesc.Count = 1;
        frameDesc.SampleDesc.Quality = 0;
        frameDesc.Usage = D3D11_USAGE_DEFAULT;
        frameDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        frameDesc.CPUAccessFlags = 0; // No CPU access for optimal GPU performance
        frameDesc.MiscFlags = 0;

        hr = d3dDevice->CreateTexture2D(&frameDesc, nullptr, m_FrameTexture.GetAddressOf());
        if (FAILED(hr)) {
            std::cerr << "Failed to create frame texture: " << std::hex << hr << std::endl;
            return false;
        }

        return true;
    }

    void OpenListener(const char* localAddr) {
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
    }

    void Loop() {
        ComPtr<ID3D11Device> d3dDevice = m_Renderer->GetD3DDevice();
        ComPtr<ID3D11DeviceContext> d3dContext = m_Renderer->GetD3DContext();

        std::cout << "Waiting for the frame..." << std::endl;

        int frames = 0;
        auto now = std::chrono::steady_clock::now();
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

                d3dContext->UpdateSubresource(m_YPlaneTexture.Get(), 0, nullptr, frameData, WIDTH, 0);
                d3dContext->UpdateSubresource(m_UVPlaneTexture.Get(), 0, nullptr, frameData + Y_PLANE_SIZE, WIDTH * 2, 0);

                m_Renderer->DecompressTexture(m_YPlaneTexture.Get(), m_UVPlaneTexture.Get(), m_FrameTexture.Get());

                m_Renderer->SetSourceSurface(m_FrameTexture.Get());

                D3D11_TEXTURE2D_DESC desc;
                m_FrameTexture->GetDesc(&desc);

                if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                    abort();
                }
            } else if (flag == 3) {
                //m_Renderer->SetSourceSurface(m_FrameTexture.Get());
                // Hmm, why bother to redraw? Just continue.
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
            /*
            auto now2 = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now2 - now).count();
            if (elapsed >= 1000000000) {
                auto flagWaitAvg = FlagWaitTotal.count() / 1000.0f /  frames;
                auto decompressAvg = DecompressTotal.count() / 1000.0f / frames;
                auto drawAvg = DrawTotal.count() / 1000.0f / frames;

                std::cout << "\r                                                                               \r" << std::flush;
                std::cout << "FPS: " << frames << " Flag: " << flagWaitAvg << " Decompress: " << decompressAvg << " Draw: " << drawAvg << std::flush;
                frames = 0;
                now = now2;

                FlagWaitTotal = std::chrono::microseconds(0);
                DecompressTotal = std::chrono::microseconds(0);
                DrawTotal = std::chrono::microseconds(0);
            }
            */

            frames++;
        }
        
        Shutdown();

        m_Renderer->Cleanup();
        m_Window->Stop();
        CoUninitialize();
    }

    void Run(const char* localAddr) {
        inputSession.Start(const_cast<char*>(localAddr));
        m_Window->RegisterRawInputCallback([this](RAWINPUT rawInput) {
            inputSession.SendEvent(rawInput);
        }, inputSession.GetCallbackEvent());
        OpenListener(localAddr);
        ExchangePeerInfo();
        Loop();
        inputSession.Stop();
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
};

// MARK: TestClient
class TestClient : public NDSessionClientBase {
public:
    TestClient() : m_CoutMutex(), inputSession(m_CoutMutex) {}

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

    void OpenConnector(const char* localAddr, const char* serverAddr) {
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
        std::this_thread::sleep_for(std::chrono::seconds(1));

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
    }

    void ExchangePeerInfo() {
        PeerInfo* myInfo = reinterpret_cast<PeerInfo*>(m_Buf);
        myInfo->remoteAddr = reinterpret_cast<UINT64>(m_Buf);
        myInfo->remoteToken = m_pMw->GetRemoteToken();

        std::cout << "My address: " << myInfo->remoteAddr << ", token: " << myInfo->remoteToken << std::endl;

        std::this_thread::sleep_for(std::chrono::seconds(1));
        
        ND2_SGE sge = { 0 };
        sge.Buffer = m_Buf;
        sge.BufferLength = sizeof(PeerInfo);
        sge.MemoryRegionToken = m_pMr->GetLocalToken();

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

        remoteInfo.remoteAddr = reinterpret_cast<PeerInfo*>(m_Buf)->remoteAddr;
        remoteInfo.remoteToken = reinterpret_cast<PeerInfo*>(m_Buf)->remoteToken;

        memset(m_Buf, 0, LENGTH_PER_FRAME);
    }

    void Loop() {
        std::cout << "Sending frames to the server." << std::endl;
        DesktopDuplication::Duplication& dupl = DesktopDuplication::Singleton<DesktopDuplication::Duplication>::Instance();

        auto now = std::chrono::steady_clock::now();

        int frames = 0;
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
            memset(m_Buf, 0, LENGTH_PER_FRAME);

            //bool success = dupl.GetStagedTexture(frame, 1000 / FPS);
            bool success = dupl.GetStagedTexture(yPlane, uvPlane, 1000 / FPS);

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

            for (unsigned int i = 0; i < HEIGHT; i++) {
                memcpy(yDst, ySrc, WIDTH);
                yDst += WIDTH;
                ySrc += mappedResource.RowPitch;
            }

            dupl.GetContext()->Unmap(yPlane, 0);
            yPlane->Release();
            yPlane = nullptr;

            dupl.GetContext()->Map(uvPlane, 0, D3D11_MAP_READ, 0, &mappedResource);
            uint8_t* uvDst = frameData + 1 + Y_PLANE_SIZE;
            uint8_t* uvSrc = reinterpret_cast<uint8_t*>(mappedResource.pData);

            for (unsigned int i = 0; i < HEIGHT / 2; i++) {
                memcpy(uvDst, uvSrc, WIDTH * 2);
                uvDst += WIDTH * 2;
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

            frames++;

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

            auto now2 = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now2 - now).count();
            if (elapsed >= 1000000000) {
                auto GetAndFlagAvg = GetAndCompressTotal.count() / 1000.0f / frames;
                auto MapAvg = MapTotal.count() / 1000.0f / frames;
                auto WriteAvg = WriteTotal.count() / 1000.0f / frames;
                auto FlagWaitAvg = FlagWaitTotal.count() / 1000.0f / frames;

                std::scoped_lock<std::mutex> lock(m_CoutMutex);
                std::cout << "\r                                                                         \r" << std::flush;
                std::cout << "FPS: " << frames << "| Comp: " << GetAndFlagAvg << " Map: " << MapAvg << " Write: " << WriteAvg << " Flag: " << FlagWaitAvg << std::flush;
                frames = 0;
                GetAndCompressTotal = std::chrono::microseconds::zero();
                MapTotal = std::chrono::microseconds::zero();
                WriteTotal = std::chrono::microseconds::zero();
                FlagWaitTotal = std::chrono::microseconds::zero();

                now = now2;
            }
        }

        Shutdown();
    }

    void Run(const char* localAddr, const char* serverAddr) {
        inputSession.Start(const_cast<char*>(localAddr), serverAddr);
        OpenConnector(localAddr, serverAddr);
        ExchangePeerInfo();
        Loop();
        inputSession.Stop();
    }

    private:
    PeerInfo remoteInfo;

    std::atomic<bool> m_isRunning = true;
    std::mutex m_CoutMutex;

    InputNDSessionClient inputSession;
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