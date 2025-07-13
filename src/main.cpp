#include "NDSession.hpp"
#include "DesktopDuplication.hpp"
#include "D2DRenderer.hpp"
#include "D2DWindow.hpp"
#include <conio.h>
#include <iostream>
#include <thread>
#include <chrono>
#include <wincodec.h>

#include <d3d11.h>
#include <DirectXTex.h>

#ifdef _DEBUG
#pragma comment(lib, "Debug/DirectXTex.lib")
#else
#pragma comment(lib, "Release/DirectXTex.lib")
#endif

constexpr char TEST_PORT[] = "54321";

#define RECV_CTXT ((void*)0x1000)
#define SEND_CTXT ((void*)0x2000)
#define READ_CTXT ((void*)0x3000)
#define WRITE_CTXT ((void*)0x4000)

#undef max
#undef min

constexpr size_t WIDTH = 2560;
constexpr size_t HEIGHT = 1440;
constexpr size_t FPS = 120;

constexpr size_t BC_BLOCK_SIZE = 16;
constexpr size_t BC_BLOCKS_WIDE = (WIDTH + 3) / 4;
constexpr size_t BC_BLOCKS_HIGH = (HEIGHT + 3) / 4;
constexpr size_t BC_COMPRESSED_SIZE = BC_BLOCKS_WIDE * BC_BLOCKS_HIGH * BC_BLOCK_SIZE;

constexpr size_t LENGTH_PER_FRAME = 1 + BC_COMPRESSED_SIZE;

static size_t maxSge = -1;

bool SaveFrameFromBuffer(const std::filesystem::path& path, const uint8_t* buffer, size_t length, unsigned int width, unsigned int height) {
    IWICImagingFactory* factory = nullptr;
    IWICBitmapEncoder* encoder = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IWICStream* stream = nullptr;

    HRESULT hr = CoInitialize(nullptr);
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

    hr = frame->WritePixels(height, width * 4, length, const_cast<unsigned char*>(buffer));
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
        
        std::vector<uint8_t> previousTex(LENGTH_PER_FRAME - 1, 0);

        auto lastDraw = std::chrono::steady_clock::time_point::max();

        while (true) {
            for (;;) {
                flag = *static_cast<unsigned char*>(m_Buf);
                if (flag == 1) {
                    std::atomic_thread_fence(std::memory_order_acquire);
                    goto emptyRun;
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

            lastDraw = std::chrono::steady_clock::now();
            renderer.SetSourceSurface(frameTex);
            renderer.Render();

            frameTex->Release();

            emptyRun:

            memset(m_Buf, 0, LENGTH_PER_FRAME);
            memset(m_Buf, 2, 1);

            if (FAILED(Write(&sge, 1, remoteInfo.remoteAddr, remoteInfo.remoteToken, 0, WRITE_CTXT))) {
                std::cerr << "Write to reset flag failed." << std::endl;
                return;
            }
            if (!WaitForCompletionAndCheckContext(WRITE_CTXT)) {
                std::cerr << "WaitForCompletion for reset flag write failed." << std::endl;
                return;
            }

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


// Simplified compress function that writes directly to m_Buf
bool CompressImageToBuf(const DirectX::ScratchImage& srcImage, void* destBuf, size_t compressedSize, ID3D11Device* device) {
    DirectX::ScratchImage compressedImage;
    auto compressStart = std::chrono::high_resolution_clock::now();
    HRESULT hr = DirectX::Compress(device, srcImage.GetImages(), srcImage.GetImageCount(), srcImage.GetMetadata(), DXGI_FORMAT_BC7_UNORM,
                                    DirectX::TEX_COMPRESS_BC7_QUICK | DirectX::TEX_COMPRESS_PARALLEL, DirectX::TEX_ALPHA_WEIGHT_DEFAULT, compressedImage);
    
    
    //HRESULT hr = DirectX::Compress(srcImage.GetImages(), srcImage.GetImageCount(), srcImage.GetMetadata(), DXGI_FORMAT_BC1_UNORM,
    //                        DirectX::TEX_COMPRESS_PARALLEL, DirectX::TEX_THRESHOLD_DEFAULT, compressedImage);
    
    if (FAILED(hr)) {
        std::cerr << "Compression failed: " << std::hex << hr << std::endl;
        return false;
    }
    auto compressEnd = std::chrono::high_resolution_clock::now();
    auto compressDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(compressEnd - compressStart).count();

    auto copyStart = std::chrono::high_resolution_clock::now();
    // Get compressed data and copy directly to buffer
    const DirectX::Image* compImg = compressedImage.GetImage(0, 0, 0);
    compressedSize = compImg->slicePitch;
    
    // Layout: [flag:1] [size:4] [compressed_data:variable]
    uint8_t* buf = static_cast<uint8_t*>(destBuf);
    buf[0] = 0; // Flag
    *reinterpret_cast<uint32_t*>(buf + 1) = static_cast<uint32_t>(compressedSize);
    memcpy(buf + 1 + sizeof(uint32_t), compImg->pixels, compressedSize);
    auto copyEnd = std::chrono::high_resolution_clock::now();
    auto copyDuration = std::chrono::duration_cast<std::chrono::nanoseconds>(copyEnd - copyStart).count();

    std::cout << "Compression: " << compressDuration / 1000000 << " ms | Copy: " << copyDuration / 1000000 << " ms" << std::flush;

    return true;
}

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

        int frames = 0;
        auto nowfps = std::chrono::steady_clock::now();

        unsigned long long totalDelay = 0;

        while (true) {
            ID3D11Texture2D* frame;

            int success = dupl.GetFrame(frame, int(1000/FPS));
            if (success == -1) {
                continue;
            } else if (success != 0) {
                std::cerr << "Failed to get frame from duplication." << std::endl;
                return;
            }

            std::cout << "                                                                                               \r" << std::flush;

            auto getTime = std::chrono::steady_clock::now();

            DirectX::ScratchImage srcImage;
            HRESULT hr = DirectX::CaptureTexture(dupl.GetDevice(), dupl.GetContext(), frame, srcImage);
            if (FAILED(hr)) {
                std::cerr << "CaptureTexture failed: " << std::hex << hr << std::endl;
                frame->Release();
                return;
            }

            auto conversionDone = std::chrono::steady_clock::now();
            auto conversionElapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(conversionDone - getTime).count();

            std::cout << "Conversion: " << conversionElapsed / 1000000.0 << " ms " << std::flush;

            CompressImageToBuf(srcImage, m_Buf, LENGTH_PER_FRAME, dupl.GetDevice());

            dupl.ReleaseFrame();

            auto compressDone = std::chrono::steady_clock::now();
            auto compressElapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(compressDone - getTime).count();
            totalDelay += compressElapsed;

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

            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - getTime).count();
            while (elapsed < 1000000000/FPS) {
                _mm_pause();
                now = std::chrono::steady_clock::now();
                elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(now - getTime).count();
            }

            // Wait for server to acknowledge the frame
            memset(m_Buf, 0, LENGTH_PER_FRAME);
            sge.Buffer = m_Buf;
            sge.BufferLength = 1;
            sge.MemoryRegionToken = m_pMr->GetLocalToken();

            while (true) {
                if (FAILED(Read(&sge, 1, remoteInfo.remoteAddr, remoteInfo.remoteToken, 0, READ_CTXT))) {
                    std::cerr << "Read failed." << std::endl;
                    return;
                }
                if (!WaitForCompletionAndCheckContext(READ_CTXT)) {
                    std::cerr << "WaitForCompletion for read failed." << std::endl;
                    return;
                }

                if (reinterpret_cast<uint8_t*>(m_Buf)[0] == 2) {
                    break;
                }
                _mm_pause();
            }

            auto now2 = std::chrono::steady_clock::now();
            auto elapsed2 = std::chrono::duration_cast<std::chrono::nanoseconds>(now2 - nowfps).count();
            if (elapsed2 <= -1000000000) {
                std::cout << "\r                " << std::flush;
                std::cout << "FPS: " << frames << " | Compression time: " << totalDelay / 1000000.0 / frames << " ms" << std::flush;
                frames = 0;
                totalDelay = 0;
                nowfps = now2;
            }
            frames++;
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