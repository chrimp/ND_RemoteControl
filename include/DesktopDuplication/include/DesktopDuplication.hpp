#include <windows.h>
#include <d3d11_4.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <string>
#include <SetupAPI.h>
#include <devguid.h>
#include <thread>
#include <atomic>
#include <mutex>
#include <filesystem>
#include <deque>
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

        ComPtr<ID3D11Device5> m_Device;
        ComPtr<ID3D11DeviceContext4> m_Context;
        ComPtr<IDXGIOutputDuplication> m_DesktopDupl;
        ComPtr<ID3D11Texture2D> m_AcquiredDesktopImage;
        ComPtr<ID3D11Texture2D> m_LastCleanTexture;
        ComPtr<ID3D11Texture2D> m_CompositionTexture;

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

        unsigned short counter = 0;
    };

    class DuplicationThread {
        public:
        DuplicationThread();
        ~DuplicationThread();

        void SetDuplication(Duplication* duplication) { m_Duplication = duplication; }

        bool Start();
        void Stop();

        void RegisterTelemetry(int* frameCount) {
            m_FrameCount = frameCount;
            *m_FrameCount = 0;
        }

        const std::deque<ComPtr<ID3D11Texture2D>>& GetFrameQueue() {
            std::scoped_lock lock(m_FrameQueueMutex);
            return m_FrameQueue;
        }

        private:
        void threadFunc();
        void threadFuncPreview();

        Duplication* m_Duplication;
        std::atomic<bool> m_Run;
        std::atomic<bool> m_ShowPreview;
        std::thread m_Thread;
        int* m_FrameCount;
        void* m_pthreadFunc;

        std::deque<ComPtr<ID3D11Texture2D>> m_FrameQueue;
        std::mutex m_FrameQueueMutex;
        size_t m_FrameQueueSize = 0;
    };

    void ChooseOutput();
    bool ChooseOutput(_Out_ UINT& adapterIndex, _Out_ UINT& outputIndex);
    int enumOutputs(IDXGIAdapter* adapter);
    std::wstring GetMonitorFriendlyName(const DXGI_OUTPUT_DESC1& desc);
    std::wstring GetMonitorNameFromEDID(const std::wstring& deviceName);

    LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
    HWND CreateWindowInstance(HINSTANCE hInstance, int nCmdShow);
}