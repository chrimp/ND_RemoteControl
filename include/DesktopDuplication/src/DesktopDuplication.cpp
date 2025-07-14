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

using namespace DesktopDuplication;

// MARK: Duplication
Duplication::Duplication() {
    m_Device = nullptr;
    m_DesktopDupl = nullptr;
    m_AcquiredDesktopImage = nullptr;
    m_Output = -1;
    m_IsDuplRunning = false;
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

    IDXGIFactory* factory = nullptr;
    HRESULT hr = CreateDXGIFactory(IID_PPV_ARGS(&factory));
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
        D3D_DRIVER_TYPE_HARDWARE,
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
    hr = dxgiAdapter->EnumOutputs(0, &dxgiOutput); // Replace 0 with user choice (WIP)
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

    frame = m_AcquiredDesktopImage.Get();

    return 0;
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
    IDXGIFactory* factory = nullptr;

    HRESULT hr = CreateDXGIFactory(IID_PPV_ARGS(&factory));
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
    IDXGIFactory* factory = nullptr;
    HRESULT hr = CreateDXGIFactory(IID_PPV_ARGS(&factory));
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