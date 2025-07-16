#ifndef D2DPRESENTATION_H
#define D2DPRESENTATION_H

#pragma once

#include <Windows.h>
#include <d2d1_3.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <cstdint>

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

using Microsoft::WRL::ComPtr;

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace D2DPresentation {
    class D2DRenderer {
        public:
        D2DRenderer();
        ~D2DRenderer();

        HRESULT Initialize(IDXGIAdapter* pAdapter, HWND hwnd, UINT width, UINT height, const D3D11_TEXTURE2D_DESC* ptextureDesc = nullptr);
        
        void SetSourceSurface(const ComPtr<ID3D11Texture2D>& sourceSurface);

        void Render();

        HRESULT Resize(UINT width, UINT height);

        void Cleanup();

        ComPtr<ID3D11Device> GetD3DDevice() const { return m_d3dDevice; }
        ComPtr<ID3D11DeviceContext> GetD3DContext() const { return m_d3dContext; }

        bool DecompressTexture(ID3D11Texture2D* yPlane, ID3D11Texture2D* uvPlane, ID3D11Texture2D* outputTexture);

        private:
        HRESULT createD3DDeviceAndSwapChain(IDXGIAdapter* pAdapter);
        HRESULT createD2DResources();
        HRESULT createSwapChainRenderTarget();
        
        void cleanup();

        HWND m_hwnd = NULL;
        UINT m_width = 0;
        UINT m_height = 0;
        bool m_isRunning = false;

        ComPtr<ID3D11Device5> m_d3dDevice;
        ComPtr<ID3D11DeviceContext4> m_d3dContext;

        ComPtr<IDXGIFactory5> m_dxgiFactory;
        ComPtr<IDXGISwapChain> m_swapChain;

        ComPtr<ID2D1Device2> m_d2dDevice;
        ComPtr<ID2D1Factory3> m_d2dFactory;
        ComPtr<ID2D1DeviceContext2> m_d2dContext;
        ComPtr<ID2D1Bitmap1> m_d2dTargetBitmap;

        ComPtr<ID3D11Texture2D> m_sharedTexture;
        ComPtr<ID2D1Bitmap1> m_d2dSourceBitmap;

        ComPtr<ID3D11ComputeShader> m_DecompressShader;

        ComPtr<ID3D11ShaderResourceView> m_YPlaneSRV;
        ComPtr<ID3D11ShaderResourceView> m_UVPlaneSRV;
        ComPtr<ID3D11UnorderedAccessView> m_outputUAV;
    };
}

#endif