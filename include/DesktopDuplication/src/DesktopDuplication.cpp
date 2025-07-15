#define UNICODE

#include "./DesktopDuplication.hpp"

#include <iostream> 
#include <conio.h>
#include <vector>
#include <initguid.h>
#include <Ntddvdeo.h>
#include <SetupAPI.h>
#include <chrono>
#include <format>
#include <wincodec.h>
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

using namespace DesktopDuplication;

// MARK: Duplication
Duplication::Duplication() {
    m_Device = nullptr;
    m_DesktopDupl = nullptr;
    m_AcquiredDesktopImage = nullptr;
    m_Output = -1;
    m_IsDuplRunning = false;

    m_CursorTexture = nullptr;
    m_CursorSRV = nullptr;
    m_FrameRTV = nullptr;
    m_AlphaBlendState = nullptr;
    m_CursorVS = nullptr;
    m_CursorPS = nullptr;
    m_CursorVertexBuffer = nullptr;
    m_CursorConstantBuffer = nullptr;
    m_CursorInputLayout = nullptr;
    m_CursorSampler = nullptr;
}

Duplication::~Duplication() {
    if (m_Device) {
        m_Device.Reset();
        m_Device = nullptr;
    }

    if (m_DesktopDupl) {
        m_DesktopDupl->ReleaseFrame();
        m_DesktopDupl.Reset();
        m_DesktopDupl = nullptr;
    }

    if (m_AcquiredDesktopImage) {
        m_AcquiredDesktopImage.Reset();
        m_AcquiredDesktopImage = nullptr;
    }
}

bool Duplication::InitDuplication() {
    if (m_Output == -1) {
        std::cout << "Output is not set. Call DesktopDuplication::ChooseOutput() to set the output." << std::endl;
        return false;
    }

    if (m_IsDuplRunning) {
        return true;
    }

    UINT flag = 0;
    #ifdef _DEBUG
    flag |= D3D11_CREATE_DEVICE_DEBUG;
    #endif

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        std::cerr << "Failed to create DXGI Factory. Reason: 0x" << std::hex << hr << std::endl;
        return false;
    }

    IDXGIAdapter* adapter = nullptr;
    hr = factory->EnumAdapters(m_AdapterIndex, &adapter);
    factory->Release();
    factory = nullptr;
    if (FAILED(hr)) {
        std::cerr << "Failed to enumerate adapters. Reason: 0x" << std::hex << hr << std::endl;
        return false;
    }

    hr = D3D11CreateDevice(
        adapter,
        D3D_DRIVER_TYPE_UNKNOWN,
        nullptr,
        flag,
        nullptr,
        0,
        D3D11_SDK_VERSION,
        &device,
        nullptr,
        &context
    );

    adapter->Release();
    adapter = nullptr;

    if (FAILED(hr)) {
        std::cerr << "Failed to create D3D11 device. Reason: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // QI for ID3D11DeviceContext4
    hr = context->QueryInterface(IID_PPV_ARGS(m_Context.GetAddressOf()));
    if (FAILED(hr)) {
        std::cerr << "Failed to query D3D11DeviceContext4 interface. Reason: 0x" << std::hex << hr << std::endl;
        return false;
    }

    context->Release();
    context = nullptr;

    // QI for ID3D11Device5
    hr = device->QueryInterface(IID_PPV_ARGS(m_Device.GetAddressOf()));
    if (FAILED(hr)) {
        std::cerr << "Failed to query D3D11Device5 interface. Reason: 0x" << std::hex << hr << std::endl;
        return false;
    }

    device->Release();
    device = nullptr;

    // QI for IDXGIDevice
    IDXGIDevice* dxgiDevice = nullptr;
    hr = m_Device->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) {
        std::cerr << "Failed to query IDXGIDevice interface. Reason: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // Get DXGI adapter
    IDXGIAdapter* dxgiAdapter = nullptr;
    hr = dxgiDevice->GetParent(IID_PPV_ARGS(&dxgiAdapter));
    dxgiDevice->Release();
    dxgiDevice = nullptr;
    if (FAILED(hr)) {
        std::cerr << "Failed to get DXGI adapter. Reason: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // Get Output (Implement enum n list for user to choose)
    IDXGIOutput* dxgiOutput = nullptr;
    hr = dxgiAdapter->EnumOutputs(m_Output, &dxgiOutput); // Replace 0 with user choice (WIP)
    dxgiAdapter->Release();
    dxgiAdapter = nullptr;
    if (FAILED(hr)) {
        std::cerr << "Failed to get DXGI output. Reason: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // QI for IDXGIOutput1
    IDXGIOutput1* dxgiOutput1 = nullptr;
    hr = dxgiOutput->QueryInterface(IID_PPV_ARGS(&dxgiOutput1));
    dxgiOutput->Release();
    dxgiOutput = nullptr;
    if (FAILED(hr)) {
        std::cerr << "Failed to query IDXGIOutput1 interface. Reason: 0x" << std::hex << hr << std::endl;
        return false;
    }

    // Create Desktop Duplication
    hr = dxgiOutput1->DuplicateOutput(m_Device.Get(), m_DesktopDupl.GetAddressOf());
    dxgiOutput1->Release();
    dxgiOutput1 = nullptr;
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_NOT_CURRENTLY_AVAILABLE) {
            std::cerr << "There are already maximum number of applications using Desktop Duplication API." << std::endl;
        } else {
            std::cerr << "Failed to create Desktop Duplication. Reason: 0x" << std::hex << hr << std::endl;
        }
        return false;
    }

    m_IsDuplRunning = true;
    return true;
}

bool Duplication::SaveFrame(const std::filesystem::path& path) {
    if (!m_IsDuplRunning) {
        std::cerr << "Desktop Duplication is not running. Call DesktopDuplication::InitDuplication() to start the duplication." << std::endl;
        return false;
    }

    ReleaseFrame();

    D3D11_TEXTURE2D_DESC desc;

    ID3D11Texture2D* stagedTexture = nullptr;
    if (!GetStagedTexture(stagedTexture)) return false;

    stagedTexture->GetDesc(&desc);
    std::vector<uint8_t> frameData(desc.Width * desc.Height * 4);

    D3D11_MAPPED_SUBRESOURCE mappedResource;
    m_Context->Map(stagedTexture, 0, D3D11_MAP_READ, 0, &mappedResource);
    
    for (unsigned int i = 0; i < desc.Height; i++) {
        memcpy(frameData.data() + i * desc.Width * 4, (uint8_t*)mappedResource.pData + i * mappedResource.RowPitch, desc.Width * 4);
    }

    m_Context->Unmap(stagedTexture, 0);
    stagedTexture->Release();
    stagedTexture = nullptr;

    unsigned int width = desc.Width;
    unsigned int height = desc.Height;

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

    hr = frame->WritePixels(height, desc.Width * 4, frameData.size(), frameData.data());
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

int Duplication::GetFrame(ID3D11Texture2D*& frame, unsigned long timeout) {
    ComPtr<IDXGIResource> desktopResource;
    DXGI_OUTDUPL_FRAME_INFO frameInfo;

    HRESULT hr = m_DesktopDupl->AcquireNextFrame(timeout, &frameInfo, &desktopResource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return -1;

    if (FAILED(hr)) {
        std::cerr << "Failed to acquire next frame. Reason: 0x" << std::hex << hr << std::endl;
        return 1;
    }

    if (m_AcquiredDesktopImage) {
        m_AcquiredDesktopImage.Reset();
    }

    // QI for ID3D11Texture2D
    hr = desktopResource->QueryInterface(IID_PPV_ARGS(m_AcquiredDesktopImage.GetAddressOf()));
    if (FAILED(hr)) {
        std::cerr << "Failed to query ID3D11Texture2D interface. Reason: 0x" << std::hex << hr << std::endl;
        return 1;
    }

    if (frameInfo.PointerPosition.Visible && frameInfo.PointerShapeBufferSize > 0) {
        CompositeCursorOnFrame(m_AcquiredDesktopImage.Get(), frameInfo);
    }

    frame = m_AcquiredDesktopImage.Get();

    return 0;
}

bool Duplication::UpdateCursorTexture(const DXGI_OUTDUPL_FRAME_INFO& frameInfo) {
    // Get cursor shape data
    std::vector<BYTE> shapeBuffer(frameInfo.PointerShapeBufferSize);
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shapeInfo;
    UINT bufferSizeRequired;
    
    HRESULT hr = m_DesktopDupl->GetFramePointerShape(
        frameInfo.PointerShapeBufferSize,
        shapeBuffer.data(),
        &bufferSizeRequired,
        &shapeInfo
    );
    
    if (FAILED(hr)) return false;
    
    // Check if shape changed
    if (memcmp(&m_LastShapeInfo, &shapeInfo, sizeof(shapeInfo)) == 0 && 
        m_LastCursorShape == shapeBuffer) {
        return true; // No change needed
    }
    
    // Create new cursor texture
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = shapeInfo.Width;
    desc.Height = shapeInfo.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    
    // Convert cursor shape data based on type
    std::vector<uint32_t> cursorPixels(shapeInfo.Width * shapeInfo.Height);
    
    if (shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR) {
        // Color cursor - direct copy
        memcpy(cursorPixels.data(), shapeBuffer.data(), shapeBuffer.size());
    }
    else if (shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME) {
        // Monochrome cursor - convert to BGRA
        ConvertMonochromeCursor(shapeBuffer.data(), cursorPixels.data(), shapeInfo);
    }
    else if (shapeInfo.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
        // Masked color cursor
        ConvertMaskedColorCursor(shapeBuffer.data(), cursorPixels.data(), shapeInfo);
    }
    
    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = cursorPixels.data();
    initData.SysMemPitch = shapeInfo.Width * 4;
    
    m_CursorTexture.Reset();
    m_CursorSRV.Reset();
    
    hr = m_Device->CreateTexture2D(&desc, &initData, m_CursorTexture.GetAddressOf());
    if (FAILED(hr)) return false;
    
    hr = m_Device->CreateShaderResourceView(m_CursorTexture.Get(), nullptr, m_CursorSRV.GetAddressOf());
    if (FAILED(hr)) return false;
    
    m_LastShapeInfo = shapeInfo;
    m_LastCursorShape = shapeBuffer;
    
    return true;
}

bool Duplication::CompositeCursorOnFrame(ID3D11Texture2D* frame, const DXGI_OUTDUPL_FRAME_INFO& frameInfo) {
    // Initialize cursor rendering resources if not done yet
    if (!m_CursorVS) {
        if (!InitializeCursorRendering()) {
            return false;
        }
    }
    
    // Update cursor texture if shape changed
    if (frameInfo.PointerShapeBufferSize > 0) {
        if (!UpdateCursorTexture(frameInfo)) {
            return false;
        }
    }
    
    // Need a cursor texture to render
    if (!m_CursorSRV) {
        return true; // No cursor to render, but not an error
    }
    
    // Create or update RTV for the frame
    m_FrameRTV.Reset();
    HRESULT hr = m_Device->CreateRenderTargetView(frame, nullptr, m_FrameRTV.GetAddressOf());
    if (FAILED(hr)) return false;
    
    // Set up rendering pipeline
    m_Context->OMSetRenderTargets(1, m_FrameRTV.GetAddressOf(), nullptr);
    m_Context->OMSetBlendState(m_AlphaBlendState.Get(), nullptr, 0xFFFFFFFF);
    
    // Set viewport
    D3D11_TEXTURE2D_DESC frameDesc;
    frame->GetDesc(&frameDesc);
    
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(frameDesc.Width);
    viewport.Height = static_cast<float>(frameDesc.Height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_Context->RSSetViewports(1, &viewport);
    
    // Set shaders and resources
    m_Context->VSSetShader(m_CursorVS.Get(), nullptr, 0);
    m_Context->PSSetShader(m_CursorPS.Get(), nullptr, 0);
    m_Context->PSSetShaderResources(0, 1, m_CursorSRV.GetAddressOf());
    m_Context->PSSetSamplers(0, 1, m_CursorSampler.GetAddressOf());
    
    // Update cursor position constants
    struct CursorConstants {
        float cursorPosX, cursorPosY;
        float cursorSizeX, cursorSizeY;
        float screenSizeX, screenSizeY;
        float hotSpotX, hotSpotY;
    } constants;
    
    constants.cursorPosX = static_cast<float>(frameInfo.PointerPosition.Position.x - m_LastShapeInfo.HotSpot.x);
    constants.cursorPosY = static_cast<float>(frameInfo.PointerPosition.Position.y - m_LastShapeInfo.HotSpot.y);
    constants.cursorSizeX = static_cast<float>(m_LastShapeInfo.Width);
    constants.cursorSizeY = static_cast<float>(m_LastShapeInfo.Height);
    constants.screenSizeX = static_cast<float>(frameDesc.Width);
    constants.screenSizeY = static_cast<float>(frameDesc.Height);
    constants.hotSpotX = static_cast<float>(m_LastShapeInfo.HotSpot.x);
    constants.hotSpotY = static_cast<float>(m_LastShapeInfo.HotSpot.y);
    
    // Update constant buffer
    D3D11_MAPPED_SUBRESOURCE mappedResource;
    hr = m_Context->Map(m_CursorConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mappedResource);
    if (SUCCEEDED(hr)) {
        memcpy(mappedResource.pData, &constants, sizeof(constants));
        m_Context->Unmap(m_CursorConstantBuffer.Get(), 0);
    }
    
    m_Context->VSSetConstantBuffers(0, 1, m_CursorConstantBuffer.GetAddressOf());
    
    // Set vertex buffer and draw cursor quad
    UINT stride = sizeof(float) * 4;
    UINT offset = 0;
    m_Context->IASetVertexBuffers(0, 1, m_CursorVertexBuffer.GetAddressOf(), &stride, &offset);
    m_Context->IASetInputLayout(m_CursorInputLayout.Get());
    m_Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    
    m_Context->Draw(4, 0);
    
    // Reset render targets to avoid issues
    ID3D11RenderTargetView* nullRTV = nullptr;
    m_Context->OMSetRenderTargets(1, &nullRTV, nullptr);
    
    return true;
}

bool Duplication::InitializeCursorRendering() {
    // Create blend state for alpha blending
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    
    HRESULT hr = m_Device->CreateBlendState(&blendDesc, m_AlphaBlendState.GetAddressOf());
    if (FAILED(hr)) return false;
    
    // Create sampler state
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    
    hr = m_Device->CreateSamplerState(&samplerDesc, m_CursorSampler.GetAddressOf());
    if (FAILED(hr)) return false;
    
    // Vertex shader source
    const char* vsSource = R"(
    struct VS_INPUT {
        float2 pos : POSITION;
        float2 uv : TEXCOORD;
    };
    
    struct VS_OUTPUT {
        float4 pos : SV_POSITION;
        float2 uv : TEXCOORD;
    };
    
    cbuffer CursorConstants : register(b0) {
        float cursorPosX;
        float cursorPosY;
        float cursorSizeX;
        float cursorSizeY;
        float screenSizeX;
        float screenSizeY;
        float hotSpotX;
        float hotSpotY;
    };
    
    VS_OUTPUT main(VS_INPUT input) {
        VS_OUTPUT output;
        
        // Transform cursor quad to screen space
        float2 screenPos = (float2(cursorPosX, cursorPosY) + input.pos * float2(cursorSizeX, cursorSizeY)) / float2(screenSizeX, screenSizeY);
        screenPos = screenPos * 2.0 - 1.0;
        screenPos.y = -screenPos.y;
        
        output.pos = float4(screenPos, 0.0, 1.0);
        output.uv = input.uv;
        return output;
    })";
    
    // Pixel shader source
    const char* psSource = R"(
    Texture2D cursorTexture : register(t0);
    SamplerState cursorSampler : register(s0);
    
    struct PS_INPUT {
        float4 pos : SV_POSITION;
        float2 uv : TEXCOORD;
    };
    
    float4 main(PS_INPUT input) : SV_TARGET {
        return cursorTexture.Sample(cursorSampler, input.uv);
    })";
    
    // Compile vertex shader
    ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
    hr = D3DCompile(vsSource, strlen(vsSource), nullptr, nullptr, nullptr, 
                   "main", "vs_5_0", 0, 0, vsBlob.GetAddressOf(), errorBlob.GetAddressOf());
    if (FAILED(hr)) {
        if (errorBlob) {
            std::cerr << "VS Compile Error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
        }
        return false;
    }
    
    // Compile pixel shader
    hr = D3DCompile(psSource, strlen(psSource), nullptr, nullptr, nullptr,
                   "main", "ps_5_0", 0, 0, psBlob.GetAddressOf(), errorBlob.GetAddressOf());
    if (FAILED(hr)) {
        if (errorBlob) {
            std::cerr << "PS Compile Error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
        }
        return false;
    }
    
    // Create shaders
    hr = m_Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), 
                                     nullptr, m_CursorVS.GetAddressOf());
    if (FAILED(hr)) return false;
    
    hr = m_Device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(),
                                    nullptr, m_CursorPS.GetAddressOf());
    if (FAILED(hr)) return false;
    
    // Create input layout
    D3D11_INPUT_ELEMENT_DESC layout[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0}
    };
    
    hr = m_Device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), 
                                    vsBlob->GetBufferSize(), m_CursorInputLayout.GetAddressOf());
    if (FAILED(hr)) return false;
    
    // Create cursor quad vertex buffer
    struct CursorVertex {
        float x, y, u, v;
    };
    
    CursorVertex vertices[] = {
        {0.0f, 0.0f, 0.0f, 0.0f},  // Top-left
        {1.0f, 0.0f, 1.0f, 0.0f},  // Top-right  
        {0.0f, 1.0f, 0.0f, 1.0f},  // Bottom-left
        {1.0f, 1.0f, 1.0f, 1.0f}   // Bottom-right
    };
    
    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.Usage = D3D11_USAGE_DEFAULT;
    bufferDesc.ByteWidth = sizeof(vertices);
    bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    
    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = vertices;
    
    hr = m_Device->CreateBuffer(&bufferDesc, &initData, m_CursorVertexBuffer.GetAddressOf());
    if (FAILED(hr)) return false;
    
    // Create constant buffer
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.ByteWidth = sizeof(float) * 8; // 8 floats for constants
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    
    hr = m_Device->CreateBuffer(&cbDesc, nullptr, m_CursorConstantBuffer.GetAddressOf());
    return SUCCEEDED(hr);
}

void Duplication::ConvertMonochromeCursor(const BYTE* srcBuffer, uint32_t* dstBuffer, const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shapeInfo) {
    UINT height = shapeInfo.Height / 2; // Monochrome cursors have AND mask + XOR mask
    UINT width = shapeInfo.Width;
    UINT pitch = shapeInfo.Pitch;
    
    const BYTE* andMask = srcBuffer;
    const BYTE* xorMask = srcBuffer + (height * pitch);
    
    for (UINT y = 0; y < height; y++) {
        for (UINT x = 0; x < width; x++) {
            UINT byteIndex = (y * pitch) + (x / 8);
            UINT bitIndex = 7 - (x % 8);
            
            bool andBit = (andMask[byteIndex] >> bitIndex) & 1;
            bool xorBit = (xorMask[byteIndex] >> bitIndex) & 1;
            
            uint32_t pixel = 0;
            if (andBit) {
                if (xorBit) {
                    // Invert background
                    pixel = 0x00FFFFFF; // White with 0 alpha (will be handled by blend)
                } else {
                    // Transparent
                    pixel = 0x00000000;
                }
            } else {
                if (xorBit) {
                    // White
                    pixel = 0xFFFFFFFF;
                } else {
                    // Black
                    pixel = 0xFF000000;
                }
            }
            
            dstBuffer[y * width + x] = pixel;
        }
    }
}

void Duplication::ConvertMaskedColorCursor(const BYTE* srcBuffer, uint32_t* dstBuffer, const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shapeInfo) {
    UINT width = shapeInfo.Width;
    UINT height = shapeInfo.Height;
    UINT pitch = shapeInfo.Pitch;
    
    // First part is the color data, second part is the mask
    const uint32_t* colorData = reinterpret_cast<const uint32_t*>(srcBuffer);
    const BYTE* maskData = srcBuffer + (height * pitch);
    
    for (UINT y = 0; y < height; y++) {
        for (UINT x = 0; x < width; x++) {
            UINT pixelIndex = y * (pitch / 4) + x;
            UINT maskByteIndex = (y * (pitch / 4) / 8) + (x / 8);
            UINT maskBitIndex = 7 - (x % 8);
            
            bool maskBit = (maskData[maskByteIndex] >> maskBitIndex) & 1;
            
            if (maskBit) {
                // Masked - make transparent
                dstBuffer[y * width + x] = 0x00000000;
            } else {
                // Use color data
                dstBuffer[y * width + x] = colorData[pixelIndex];
            }
        }
    }
}

void Duplication::ReleaseFrame() {
    HRESULT hr = m_DesktopDupl->ReleaseFrame();
    if (FAILED(hr)) {
        if (hr == DXGI_ERROR_INVALID_CALL) {} // Frame was already released
        else {
            std::cerr << "Failed to release frame. Reason: 0x" << std::hex << hr << std::endl;
            throw new std::exception();
        }
    }

    if (m_AcquiredDesktopImage) {
        m_AcquiredDesktopImage.Reset();
    }

    return;
}

bool Duplication::GetStagedTexture(_Out_ ID3D11Texture2D*& dst) {
    ID3D11Texture2D* frame = nullptr;
    int result = GetFrame(frame);

    switch (result) {
        case 1:
            #ifdef _DEBUG
            abort();
            #endif
            return false;
        case -1:
            return false;
    }

    D3D11_TEXTURE2D_DESC desc;
    frame->GetDesc(&desc);

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.MiscFlags = 0;

    m_Device->CreateTexture2D(&desc, nullptr, &dst);
    m_Context->CopyResource(dst, frame);
    ReleaseFrame();

    return true;
}

bool Duplication::GetStagedTexture(_Out_ ID3D11Texture2D*& dst, _In_ unsigned long timeout) {
    ID3D11Texture2D* frame = nullptr;
    int result = GetFrame(frame, timeout);

    switch (result) {
        case 1:
            #ifdef _DEBUG
            abort();
            #endif
            return false;
        case -1:
            return false;
    }

    D3D11_TEXTURE2D_DESC desc;
    frame->GetDesc(&desc);

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.MiscFlags = 0;

    m_Device->CreateTexture2D(&desc, nullptr, &dst);
    m_Context->CopyResource(dst, frame);
    ReleaseFrame();

    dst->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) abort();

    return true;
}

void Duplication::SetOutput(UINT adapterIndex, UINT outputIndex) {
    m_Output = outputIndex;
    m_AdapterIndex = adapterIndex;
}

// MARK: DuplicationThread
DuplicationThread::DuplicationThread() : m_Duplication(nullptr), m_Run(false), m_FrameCount(nullptr) {}

DuplicationThread::~DuplicationThread() {
    Stop();
}

bool DuplicationThread::Start() {
    // Condition checks
    if (m_Run) return true;
    if (!m_Duplication) {
        std::cerr << "DuplicationThread doesn't have Duplication instance." << std::endl;
        return false;
    }
    if (!m_Duplication->IsOutputSet()) {
        std::cerr << "DuplicationThread's instance doesn't have output." << std::endl;
        return false;
    }

    bool start = m_Duplication->InitDuplication();
    if (!start) return false;

    m_Run = true;

    m_Thread = std::thread(&DuplicationThread::threadFunc, this);

    return true;
}

void DuplicationThread::Stop() {
    // Condition checks
    if (!m_Run) return;

    m_Run = false;

    if (m_Thread.joinable()) {
        m_Thread.join();
    }

    return;
}

void DuplicationThread::threadFunc() {
	std::chrono::time_point<std::chrono::high_resolution_clock> lastTime = std::chrono::high_resolution_clock::now();
    while (m_Run) {
        // Get duplicated surface without staging
        ID3D11Texture2D* frame = nullptr;
        int result = m_Duplication->GetFrame(frame);

        switch (result) {
            case 1:
                m_Run = false;
                #ifdef _DEBUG
                abort();
                #endif
                return;
            case -1:
                continue; // Should preserve the previous frame; duplication is timed out
        }

        D3D11_TEXTURE2D_DESC desc;
        frame->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        desc.CPUAccessFlags = 0;
        desc.MiscFlags = 0;

        ComPtr<ID3D11Texture2D> copyDest;
        m_Duplication->GetDevice()->CreateTexture2D(&desc, nullptr, copyDest.GetAddressOf());
        m_Duplication->GetContext()->CopyResource(copyDest.Get(), frame);

        {
            std::scoped_lock lock(m_FrameQueueMutex);
            m_FrameQueue.push_back(std::move(copyDest));
            if (m_FrameQueue.size() > 0 && m_FrameQueue.size() > m_FrameQueueSize) {
                m_FrameQueue.pop_front();
            }
        }

        m_Duplication->ReleaseFrame();
		*m_FrameCount = *m_FrameCount + 1;
    }
}

// MARK: Utils
bool DesktopDuplication::ChooseOutput(_Out_ UINT& adapterIndex, _Out_ UINT& outputIndex) {
    // Currently assuming there is only one adapter
    IDXGIFactory1* factory = nullptr;

    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        std::cerr << "Failed to create DXGI Factory. Reason: 0x" << std::hex << hr << std::endl;
        return false;
    }

    adapterIndex = 0;
    IDXGIAdapter* adapter = nullptr;

    hr = factory->EnumAdapters(adapterIndex, &adapter);
    factory->Release();
    factory = nullptr;
    if (FAILED(hr)) {
        std::cerr << "Failed to enumerate adapters. Reason: 0x" << std::hex << hr << std::endl;
        return false;
    }

    UINT lastOutputNum = enumOutputs(adapter);
    adapter->Release();
    adapter = nullptr;

    bool validOutput = false;
    while (!validOutput) {
        std::cout << std::endl;
        std::cout << "Choose an output to capture: ";
        char input = _getch();
        UINT output = input - '0';

        if (output >= lastOutputNum || output < 0) {
            std::cout << "Invalid output number. Please choose a valid output." << std::endl;
            std::cout << "Press any key to continue..." << std::endl;
            _getch();
            std::cout << "\033[2A"
                      << "\033[K"
                      << "\033[1B"
                      << "\033[K"
                      << "\033[1A";
        } else {
            validOutput = true;
            outputIndex = output;
            system("cls");
        }
    }

    return true;
}

void DesktopDuplication::ChooseOutput() {
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        std::cerr << "Failed to create DXGI Factory. Reason: 0x" << std::hex << hr << std::endl;
        return;
    }

    UINT adapterIndex = 0;
    IDXGIAdapter* tempAdapter = nullptr;
    std::vector<IDXGIAdapter*> adapters;
    std::vector<std::wstring> adapterDescriptions;

    while (factory->EnumAdapters(adapterIndex, &tempAdapter) != DXGI_ERROR_NOT_FOUND) {
        adapters.push_back(tempAdapter);
        DXGI_ADAPTER_DESC desc;
        tempAdapter->GetDesc(&desc);

        std::wstringstream wss;
        wss << L"Adapter " << adapterIndex << L": " << desc.Description;
        adapterDescriptions.push_back(wss.str());

        adapterIndex++;
    }

    if (adapters.empty()) {
        std::cerr << "No adapters found." << std::endl;
        factory->Release();
        return;
    }

    UINT selectedAdapterIndex = 0;
    int selectedOutputIndex = -1;
    bool selectionComplete = false;

    while (!selectionComplete) {
        system("cls");
        for (const auto& desc : adapterDescriptions) {
            std::wcout << desc << std::endl;
        }
        std::cout << std::endl;
        std::cout << "\nChoose an adapter to use: ";
        char input = _getch();
        UINT choice = input - '0';

        if (choice >= adapters.size()) {
            std::cout << "Invalid adapter number. Press any key to continue..." << std::endl;
            _getch();
            continue;
        }
        
        system("cls");
        std::cout << "Selected adapter #" << choice << ".\n" << std::endl;

        selectedOutputIndex = enumOutputs(adapters[choice]);

        if (selectedOutputIndex == -1) {
            std::cout << "\nThis adapter has no outputs. Please choose another one." << std::endl;
        }
        else if (selectedOutputIndex == -2) continue;
        else {
            selectionComplete = true;
            selectedAdapterIndex = choice;
        }
    }

    // Set the chosen adapter and output in the singleton instance.
    Singleton<Duplication>::Instance().SetOutput(selectedAdapterIndex, selectedOutputIndex);
    system("cls");

    // Release all enumerated adapters now that we are done.
    for (auto& adapter : adapters) {
        adapter->Release();
    }
    adapters.clear();

    if (factory) {
        factory->Release();
    }
}

int DesktopDuplication::enumOutputs(IDXGIAdapter* adapter) {
    UINT outputIndex = 0;
    IDXGIOutput* output = nullptr;
    std::vector<DXGI_OUTPUT_DESC1> outputDescs;

    // First, enumerate and store all available outputs for the adapter.
    while (adapter->EnumOutputs(outputIndex, &output) != DXGI_ERROR_NOT_FOUND) {
        IDXGIOutput6* output6 = nullptr;
        HRESULT hr = output->QueryInterface(IID_PPV_ARGS(&output6));
        if (SUCCEEDED(hr)) {
            DXGI_OUTPUT_DESC1 desc;
            if (SUCCEEDED(output6->GetDesc1(&desc))) {
                outputDescs.push_back(desc);
            }
            output6->Release();
        }
        output->Release();
        output = nullptr;
        outputIndex++;
    }

    // If no outputs were found, return -1 to signal failure.
    if (outputDescs.empty()) {
        return -1;
    }

    // Display the found outputs to the user.
    for (size_t i = 0; i < outputDescs.size(); ++i) {
        std::wstring monitorName = GetMonitorFriendlyName(outputDescs[i]);
        std::wcout << L"Output " << i << L": " << monitorName << L"\n";
        
        DEVMODE devMode = {};
        devMode.dmSize = sizeof(DEVMODE);
        if (EnumDisplaySettings(outputDescs[i].DeviceName, ENUM_CURRENT_SETTINGS, &devMode)) {
            std::cout << "  Current Mode: " << devMode.dmPelsWidth << "x" << devMode.dmPelsHeight << "@" << devMode.dmDisplayFrequency << "Hz" << std::endl;
        }
    }

    // Get the user's selection.
    bool validOutput = false;
    int selectedOutput = -1;
    while (!validOutput) {
        std::cout << std::endl;
        std::cout << "Choose an output to capture (Press q to return): ";
        char input = _getch();

        if (input == 'q') {
            return -2;
        }

        int choice = input - '0';

        if (choice < 0 || choice >= static_cast<int>(outputDescs.size())) {
            std::cout << "Invalid output number. Please choose a valid output." << std::endl;
            std::cout << "Press any key to continue..." << std::endl;
            _getch();
            std::cout << "\033[2A"
                      << "\033[K"
                      << "\033[1B"
                      << "\033[K"
                      << "\033[1A";
        } else {
            validOutput = true;
            selectedOutput = choice;
        }
    }

    return selectedOutput;
}

std::wstring DesktopDuplication::GetMonitorNameFromEDID(const std::wstring& deviceName) {
    std::vector<BYTE> edid;

    DISPLAY_DEVICE dd;
    dd.cb = sizeof(DISPLAY_DEVICE);

    if (EnumDisplayDevices(deviceName.c_str(), 0, &dd, 0)) {
        HDEVINFO devInfo = SetupDiGetClassDevsEx(&GUID_DEVINTERFACE_MONITOR, NULL, NULL, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE, NULL, NULL, NULL);
    
        if (devInfo == INVALID_HANDLE_VALUE) {
            return L"";
        }

        SP_DEVINFO_DATA devInfoData;
        devInfoData.cbSize = sizeof(SP_DEVINFO_DATA);

        if (SetupDiEnumDeviceInfo(devInfo, 0, &devInfoData)) {
            HKEY hDevRegKey = SetupDiOpenDevRegKey(devInfo, &devInfoData, DICS_FLAG_GLOBAL, 0, DIREG_DEV, KEY_READ);

            if (hDevRegKey == INVALID_HANDLE_VALUE) {
                SetupDiDestroyDeviceInfoList(devInfo);
                return L"";
            }

            DWORD edidSize = 0;
            RegQueryValueEx(hDevRegKey, L"EDID", NULL, NULL, NULL, &edidSize);

            if (edidSize != 0) {
                edid.resize(edidSize);
                RegQueryValueEx(hDevRegKey, L"EDID", NULL, NULL, edid.data(), &edidSize);
            }

            RegCloseKey(hDevRegKey);
        }

        SetupDiDestroyDeviceInfoList(devInfo);
    }

    if (edid.size() == 0) {
        return L"";
    }

    // Parse EDID (Range: 54-71, 72-89, 90-107, 108-125) (Look for: 0x00 0x00 0x00 0xFC)
    std::wstring monitorName = L"";
    std::vector<std::pair<size_t, size_t>> ranges = {{54, 71}, {72, 89}, {90, 107}, {108, 125}};
    
    for (const auto& range : ranges) {
        for (size_t i = range.first; i <= range.second && i + 3 < edid.size(); i++) {
            if (edid[i] == 0x00 && edid[i + 1] == 0x00 && edid[i + 2] == 0x00 && edid[i + 3] == 0xFC) {
                i += 5;
                while (i < edid.size() && edid[i] != 0x0A) {
                    monitorName += edid[i];
                    i++;
                }
                break;
            }
        }
        if (!monitorName.empty()) {
            break;
        }
    }

    return monitorName;
}

std::wstring DesktopDuplication::GetMonitorFriendlyName(const DXGI_OUTPUT_DESC1& desc) {
    // Generic PnP Monitor

    std::wstring monitorName = L"Unknown";

    DISPLAY_DEVICE displayDevice;
    displayDevice.cb = sizeof(DISPLAY_DEVICE);

    if (EnumDisplayDevices(desc.DeviceName, 0, &displayDevice, 0)) {
        monitorName = displayDevice.DeviceString;
    }

    if (monitorName == L"Generic PnP Monitor") {
        monitorName = GetMonitorNameFromEDID(desc.DeviceName);
    }
    
    return monitorName;
}