#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <string>
#include <SetupAPI.h>
#include <devguid.h>
#include <filesystem>
#include <d2d1_3.h>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "SetupAPI.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "d2d1.lib")

using Microsoft::WRL::ComPtr;

namespace DesktopDuplication {
    template <typename T>
    class Singleton {
        public:
        static T& Instance() {
            static T instance;
            return instance;
        }

        Singleton(const Singleton&) = delete;
        Singleton& operator=(const Singleton&) = delete;

        protected:
        Singleton() = default;
        ~Singleton() = default;
    };

    class Duplication {
        public:
        Duplication();
        ~Duplication();

        ID3D11Device5* GetDevice() { return m_Device.Get(); }
        ID3D11DeviceContext4* GetContext() { return m_Context.Get(); }

        void SetOutput(UINT adapterIndex, UINT outputIndex);
        void GetTelemetry(_Out_ unsigned long long& frameCount, _Out_ unsigned int& framePerUnit);

        bool InitDuplication();
        bool IsOutputSet() { return m_Output != -1; }
        bool SaveFrame(const std::filesystem::path& path);
        bool GetStagedTexture(_Out_ ID3D11Texture2D*& dst);
        bool GetStagedTexture(_Out_ ID3D11Texture2D*& dst, _In_ unsigned long timeout);
        bool GetStagedTexture(_Out_ ID3D11Texture2D*& YPlane, _Out_ ID3D11Texture2D*& UVPlane, _In_ unsigned long timeout = 16);

        int GetFrame(_Out_ ID3D11Texture2D*& frame, _In_ unsigned long timemout = 16);

        void ReleaseFrame();

        private:
        int GetAndCompressTexture(unsigned long timeout);
        bool RecreateOutputDuplication();

        ComPtr<ID3D11Device5> m_Device;
        ComPtr<ID3D11DeviceContext4> m_Context;
        ComPtr<IDXGIOutputDuplication> m_DesktopDupl;
        ComPtr<ID3D11Texture2D> m_AcquiredDesktopImage;
        ComPtr<ID3D11Texture2D> m_LastCleanTexture;
        ComPtr<ID3D11Texture2D> m_CompositionTexture;
        ComPtr<IDXGIOutput1> m_DXGIOutput;

        UINT m_Output;
        UINT m_AdapterIndex;
        bool m_IsDuplRunning;

        ComPtr<ID2D1Factory3> m_D2DFactory;
        ComPtr<ID2D1Device2> m_D2DDevice;
        ComPtr<ID2D1DeviceContext2> m_D2DContext;
        ComPtr<ID2D1SolidColorBrush> m_BlackBrush;
        ComPtr<ID2D1SolidColorBrush> m_WhiteBrush;

        std::vector<uint8_t> m_CursorShape;
        DXGI_OUTDUPL_POINTER_SHAPE_INFO m_CursorShapeInfo;

        void CompressTexture(ID3D11Texture2D* inputTexture);

        ComPtr<ID3D11Texture2D> m_YPlaneTexture;  // Stores the Y (luminance) plane
        ComPtr<ID3D11Texture2D> m_UVPlaneTexture; // Stores the UV (chroma) plane
        ComPtr<ID3D11ComputeShader> m_CompressShader;   // Shader for 4:4:0 compression
        ComPtr<ID3D11Buffer> m_ConstantsBuffer; // Buffer for shader constants (e.g., width, height)

        ComPtr<ID3D11Texture2D> m_SRTexture;
        ComPtr<ID3D11Fence> m_Fence;

        ComPtr<ID3D11ShaderResourceView> inputSRV;
        ComPtr<ID3D11UnorderedAccessView> yPlaneUAV;
        ComPtr<ID3D11UnorderedAccessView> uvPlaneUAV;

        unsigned short counter = 0;
    };

    void ChooseOutput(_Out_ unsigned short& width, _Out_ unsigned short& height, _Out_ unsigned short& refreshRate);
    int enumOutputs(IDXGIAdapter* adapter, unsigned short& width, unsigned short& height, unsigned short& refreshRate);
    std::wstring GetMonitorFriendlyName(const DXGI_OUTPUT_DESC1& desc);
    std::wstring GetMonitorNameFromEDID(const std::wstring& deviceName);
}