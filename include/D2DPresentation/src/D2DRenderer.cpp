#include "../include/D2DRenderer.hpp"
#include <dxgi1_2.h>
#include <exception>

#undef min
#undef max

#include <algorithm>
#include <iostream>
#include <d3dcompiler.h>

#pragma comment(lib, "d3dcompiler.lib")

/*
* ======================================================================
* NOTICE: This code contains a mix of human-authored and AI-generated
* content with varying levels of AI involvement.
*
* Some portions were created or modified by AI based on requirements
* or specifications. The extent and nature of AI contributions may
* vary throughout the codebase and are not explicitly marked.
*
* The code is provided as-is without warranty or guarantee of accuracy.
* AI-contributed portions have not been thoroughly tested and may
* contain errors, omissions, or implementation inconsistencies.
*
* You are responsible for comprehensive review, testing, and validation
* of all code before use. Exercise particular caution during integration
* and follow established development best practices.
* ======================================================================
*/

using namespace D2DPresentation;

D2DRenderer::D2DRenderer() {}

D2DRenderer::~D2DRenderer() {
    Cleanup();
}

HRESULT D2DRenderer::Initialize(IDXGIAdapter* pAdapter, HWND hwnd, UINT width, UINT height, const D3D11_TEXTURE2D_DESC* ptextureDesc) {
    m_hwnd = hwnd;
    m_width = width;
    m_height = height;

    if (!hwnd || width == 0 || height == 0) {
        return E_INVALIDARG;
    }

    /*
    DXGI_FORMAT format;
    DXGI_COLOR_SPACE_TYPE colorSpace;

    if (!ptextureDesc) {
        format = DXGI_FORMAT_B8G8R8A8_UNORM;
        colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    } else {
        format = ptextureDesc->Format;
        switch (format) {
            case DXGI_FORMAT_R10G10B10A2_UNORM:
                colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
                break;
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709; // HDR10
                break;
            default:
                colorSpace = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709; // Default to a common SDR format
                break;
        }
    }
    */

    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(m_dxgiFactory.GetAddressOf()));
    if (FAILED(hr)) {
        abort();
        return hr;
    }

    hr = createD3DDeviceAndSwapChain(pAdapter);
    if (FAILED(hr)) {
        abort();
        return hr;
    }

    hr = createD2DResources();
    if (FAILED(hr)) {
        abort();
        return hr;
    }

    hr = createSwapChainRenderTarget();
    if (FAILED(hr)) {
        abort();
        return hr;
    }

    m_d2dContext->BeginDraw();
    m_d2dContext->SetTarget(m_d2dTargetBitmap.Get());
    m_d2dContext->Clear(D2D1::ColorF(D2D1::ColorF::Black));
    m_d2dContext->EndDraw();
    m_swapChain->Present(1, 0);

    m_isRunning = true;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = m_width;
    desc.Height = m_height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.SampleDesc.Quality = 0;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.CPUAccessFlags = 0;
    desc.MiscFlags = 0;

    hr = m_d3dDevice->CreateTexture2D(&desc, nullptr, m_sharedTexture.GetAddressOf());
    if (FAILED(hr)) {
        abort();
        return hr;
    }

    // Compile shader
    ComPtr<ID3DBlob> shaderBlob;
    ComPtr<ID3DBlob> errorBlob;
    hr = D3DCompileFromFile(L"shaders/440_2BGRA.hlsl", nullptr, nullptr, "main", "cs_5_0", 0, 0, &shaderBlob, &errorBlob);
    if (FAILED(hr)) {
        if (errorBlob) {
            std::cerr << "Shader compilation error: " << static_cast<const char*>(errorBlob->GetBufferPointer()) << std::endl;
        }
        abort();
        return hr;
    }

    hr = m_d3dDevice->CreateComputeShader(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize(), nullptr, m_DecompressShader.GetAddressOf());
    if (hr == S_FALSE || FAILED(hr)) {
        std::cerr << "Failed to create compute shader: " << std::hex << hr << std::endl;
        abort();
        return hr;
    }

    return S_OK;
}

void D2DRenderer::SetSourceSurface(const ComPtr<ID3D11Texture2D>& sourceSurface) {
    m_d2dSourceBitmap.Reset();

    if (!sourceSurface) {
        m_sharedTexture.Reset();
        return;
    }

    D3D11_TEXTURE2D_DESC desc;
    sourceSurface->GetDesc(&desc);

    m_d3dContext->CopyResource(m_sharedTexture.Get(), sourceSurface.Get());


    D2D1_BITMAP_PROPERTIES1 bitmapProperties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_PREMULTIPLIED)
    );

    ComPtr<IDXGISurface> dxgiSurface;
    HRESULT hr = m_sharedTexture.As(&dxgiSurface);
    if (FAILED(hr)) {
        m_sharedTexture.Reset();
        return;
    }

    hr = m_d2dContext->CreateBitmapFromDxgiSurface(dxgiSurface.Get(), &bitmapProperties, m_d2dSourceBitmap.GetAddressOf());
    if (FAILED(hr)) {
        m_d2dSourceBitmap.Reset();
    }
}

bool D2DRenderer::DecompressTexture(ID3D11Texture2D* yPlane, ID3D11Texture2D* uvPlane, ID3D11Texture2D* outputTexture) {
    D3D11_QUERY_DESC queryDesc = {};
    queryDesc.Query = D3D11_QUERY_EVENT;
    queryDesc.MiscFlags = 0;
    ComPtr<ID3D11Query> query;
    HRESULT hr = m_d3dDevice->CreateQuery(&queryDesc, query.GetAddressOf());
    if (FAILED(hr)) {
        std::cerr << "Failed to create query for decompression. Reason: 0x" << std::hex << hr << std::endl;
        throw std::exception();
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    srvDesc.Texture2D.MostDetailedMip = 0;
    
    ComPtr<ID3D11ShaderResourceView> ySRV;
    hr = m_d3dDevice->CreateShaderResourceView(yPlane, &srvDesc, &ySRV);
    if (FAILED(hr)) return hr;
    
    srvDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    ComPtr<ID3D11ShaderResourceView> uvSRV;
    hr = m_d3dDevice->CreateShaderResourceView(uvPlane, &srvDesc, &uvSRV);
    if (FAILED(hr)) return hr;
    
    // Create unordered access view for output texture
    D3D11_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    uavDesc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    
    ComPtr<ID3D11UnorderedAccessView> outputUAV;
    hr = m_d3dDevice->CreateUnorderedAccessView(outputTexture, &uavDesc, &outputUAV);
    if (FAILED(hr)) return hr;
    
    // Set compute shader and resources
    m_d3dContext->CSSetShader(m_DecompressShader.Get(), nullptr, 0);
    
    ID3D11ShaderResourceView* srvs[] = { ySRV.Get(), uvSRV.Get() };
    m_d3dContext->CSSetShaderResources(0, 2, srvs);
    
    ID3D11UnorderedAccessView* uavs[] = { outputUAV.Get() };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, uavs, nullptr);
    
    // Dispatch compute shader (assuming 1920x1080 resolution)
    UINT dispatchX = (m_width + 15) / 16;  // Round up for 16x16 thread groups
    UINT dispatchY = (m_height + 15) / 16;
    m_d3dContext->Dispatch(dispatchX, dispatchY, 1);
    
    m_d3dContext->End(query.Get());

    // Cleanup
    ID3D11ShaderResourceView* nullSRV[] = { nullptr, nullptr };
    m_d3dContext->CSSetShaderResources(0, 2, nullSRV);
    
    ID3D11UnorderedAccessView* nullUAV[] = { nullptr };
    m_d3dContext->CSSetUnorderedAccessViews(0, 1, nullUAV, nullptr);
    
    m_d3dContext->CSSetShader(nullptr, nullptr, 0);
    
    while (m_d3dContext->GetData(query.Get(), nullptr, 0, 0) == S_FALSE) _mm_pause();

    return S_OK;
}

void D2DRenderer::Render() {
    if (!m_isRunning) return;

    WaitForSingleObjectEx(m_frameWaitableObject, 100, true);

    m_d2dContext->BeginDraw();
    m_d2dContext->SetTarget(m_d2dTargetBitmap.Get());
    m_d2dContext->Clear(D2D1::ColorF(D2D1::ColorF::Black));

    if (m_d2dSourceBitmap) {
        D2D1_SIZE_F sourceBitmapSize = m_d2dSourceBitmap->GetSize();
        float sourceWidth = sourceBitmapSize.width;
        float sourceHeight = sourceBitmapSize.height;

        float scaleX = static_cast<float>(m_width) / sourceWidth;
        float scaleY = static_cast<float>(m_height) / sourceHeight;
        float scale = std::min(scaleX, scaleY);

        float scaleWidth = sourceWidth * scale;
        float scaleHeight = sourceHeight * scale;
        float offsetX = (static_cast<float>(m_width) - scaleWidth) / 2.0f;
        float offsetY = (static_cast<float>(m_height) - scaleHeight) / 2.0f;

        D2D1_RECT_F destRect = D2D1::RectF(offsetX, offsetY, offsetX + scaleWidth, offsetY + scaleHeight);

        D2D1_BITMAP_INTERPOLATION_MODE interpolationMode;

        if (scale >= 1.0f) {
            float integerScale = floorf(scale);
            if (integerScale >= 1.0f && fabsf(scale - integerScale) < 0.01f) {
                interpolationMode = D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR;

                float exactWidth = sourceWidth * integerScale;
                float exactHeight = sourceHeight * integerScale;
                float exactOffsetX = (static_cast<float>(m_width) - exactWidth) / 2.0f;
                float exactOffsetY = (static_cast<float>(m_height) - exactHeight) / 2.0f;
                destRect = D2D1::RectF(exactOffsetX, exactOffsetY, exactOffsetX + exactWidth, exactOffsetY + exactHeight);
            } else {
                interpolationMode = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
            }
        } else {
            interpolationMode = D2D1_BITMAP_INTERPOLATION_MODE_LINEAR;
        }

        m_d2dContext->DrawBitmap(m_d2dSourceBitmap.Get(), destRect, 1.0f, interpolationMode);
    }

    HRESULT hr = m_d2dContext->EndDraw();
    if (FAILED(hr)) {
        if (hr == D2DERR_RECREATE_TARGET) {
            createSwapChainRenderTarget();
        } else {
            throw std::exception();
        }
    }

    hr = m_swapChain->Present(0, 0);
    if (FAILED(hr)) {
        throw std::exception();
    }
}

// Not in use && Do not use
HRESULT D2DRenderer::Resize(UINT width, UINT height) {
    if (width == 0 || height == 0) {
        return E_INVALIDARG;
    }

    if (m_width == width && m_height == height) {
        return S_OK; // No change in size
    }

    m_width = width;
    m_height = height;

    m_d2dContext->SetTarget(nullptr);
    m_d2dTargetBitmap.Reset();

    HRESULT hr = m_swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) {
        return hr;
    }

    hr = createSwapChainRenderTarget();
    if (FAILED(hr)) {
        return hr;
    }

    return S_OK;
}

void D2DRenderer::Cleanup() {
    m_isRunning = false;
    cleanup();
}

HRESULT D2DRenderer::createD3DDeviceAndSwapChain(IDXGIAdapter* pAdapter) {

    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = 2;
    swapChainDesc.Width = m_width;
    swapChainDesc.Height = m_height;
    swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    //swapChainDesc.Scaling = DXGI_SCALING_ASPECT_RATIO_STRETCH;
    swapChainDesc.BufferCount = 2;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

    UINT flags = 0;
    #ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
    #endif

    flags |= D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_1}; // Does not work with 11.0 or lower.

    ID3D11Device* d3dDevice = nullptr;
    ID3D11DeviceContext* d3dContext = nullptr;

    /*
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        featureLevels,
        1,
        D3D11_SDK_VERSION,
        &swapChainDesc,
        m_swapChain.GetAddressOf(),
        &d3dDevice,
        nullptr,
        &d3dContext
    );
    */

    HRESULT hr = D3D11CreateDevice(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        featureLevels,
        1,
        D3D11_SDK_VERSION,
        &d3dDevice,
        nullptr,
        &d3dContext
    );

    if (FAILED(hr)) {
        #ifdef _DEBUG
        abort();
        #endif
        return hr;
    }

    d3dDevice->QueryInterface(IID_PPV_ARGS(m_d3dDevice.GetAddressOf()));
    d3dContext->QueryInterface(IID_PPV_ARGS(m_d3dContext.GetAddressOf()));

    d3dDevice->Release();
    d3dContext->Release();

    IDXGISwapChain1* swapChain = nullptr;

    hr = m_dxgiFactory->CreateSwapChainForHwnd(m_d3dDevice.Get(), m_hwnd, &swapChainDesc, nullptr, nullptr, &swapChain);
    if (FAILED(hr)) {
        #ifdef _DEBUG
        abort();
        #endif
        return hr;
    }

    hr = swapChain->QueryInterface(IID_PPV_ARGS(m_swapChain.GetAddressOf()));
    swapChain->Release();
    if (FAILED(hr)) {
        #ifdef _DEBUG
        abort();
        #endif
        return hr;
    }

    hr = m_swapChain->SetMaximumFrameLatency(1);
    if (FAILED(hr)) {
        #ifdef _DEBUG
        abort();
        #endif
        return hr;
    }

    m_frameWaitableObject = m_swapChain->GetFrameLatencyWaitableObject();

    return S_OK;
}

HRESULT D2DRenderer::createD2DResources() {
    D2D1_FACTORY_OPTIONS d2dFactoryOptions = {};
    #ifdef _DEBUG
    d2dFactoryOptions.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
    #endif

    HRESULT hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_MULTI_THREADED,
        __uuidof(ID2D1Factory3),
        &d2dFactoryOptions,
        reinterpret_cast<void**>(m_d2dFactory.GetAddressOf())
    );

    if (FAILED(hr)) {
        return hr;
    }

    IDXGIDevice* dxgiDevice = nullptr;
    hr = m_d3dDevice->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
    if (FAILED(hr)) {
        return hr;
    }

    hr = m_d2dFactory->CreateDevice(dxgiDevice, m_d2dDevice.GetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }

    hr = m_d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, m_d2dContext.GetAddressOf());
    if (FAILED(hr)) {
        return hr;
    }

    dxgiDevice->Release();
    return S_OK;
}

HRESULT D2DRenderer::createSwapChainRenderTarget() {
    m_d2dTargetBitmap.Reset();
    m_d2dContext->SetTarget(nullptr);

    ComPtr<IDXGISurface> dxgiBackBuffer;
    HRESULT hr = m_swapChain->GetBuffer(0, IID_PPV_ARGS(&dxgiBackBuffer));
    if (FAILED(hr)) {
        return hr;
    }

    D2D1_BITMAP_PROPERTIES1 bitmapProperties = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        96.0f, 96.0f
    );

    hr = m_d2dContext->CreateBitmapFromDxgiSurface(
        dxgiBackBuffer.Get(),
        &bitmapProperties,
        m_d2dTargetBitmap.GetAddressOf()
    );

    if (FAILED(hr)) {
        m_d2dTargetBitmap.Reset();
        return hr;
    }

    return S_OK;
}

void D2DRenderer::cleanup() {
    m_isRunning = false;

    if (m_d2dContext) {
        m_d2dContext->SetTarget(nullptr);
    }

    m_d2dContext.Reset();
    m_d2dDevice.Reset();
    m_d2dFactory.Reset();
    m_swapChain.Reset();
    m_d3dContext.Reset();
    m_d3dDevice.Reset();
    m_dxgiFactory.Reset();
    m_d2dTargetBitmap.Reset();
    m_d2dSourceBitmap.Reset();
    m_sharedTexture.Reset();
}