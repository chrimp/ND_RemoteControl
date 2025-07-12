#include "../include/D2DWindow.hpp"
#include <objbase.h>

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

D2DWindow::D2DWindow(
    LPCTSTR title,
    UINT width,
    UINT height
) {
    m_windowTitle = title;
    m_width = width;
    m_height = height;
}

D2DWindow::~D2DWindow() {
    Stop();
}

void D2DWindow::WindowThread(
    LPCTSTR title,
    UINT width,
    UINT height
) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_UNAWARE);

    HRESULT hrCoInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hrCoInit)) {
        throw std::exception();
    }

    HINSTANCE hInstance = GetModuleHandle(NULL);
    if (!hInstance) {
        CoUninitialize();
        throw std::exception();
    }

    m_hInstance = hInstance;
    
    HRESULT hr = RegisterWindowClass(hInstance);
    if (FAILED(hr)) {
        throw std::exception();
    }

    m_hwnd = CreateWindowEx(
        0,
        m_className.c_str(),
        title,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height,
        NULL, NULL, hInstance, this
    );

    if (!m_hwnd) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        throw std::exception();
    }

    ShowWindow(m_hwnd, SW_SHOW);
    UpdateWindow(m_hwnd);

    MSG msg;
    while (m_isRunning) {
        while (GetMessage(&msg, NULL, 0, 0)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
    }

    if(SUCCEEDED(hrCoInit)) {
        CoUninitialize();
    }
}

void D2DWindow::Start() {
    if (m_isRunning) {
        return;
    }

    m_isRunning = true;

    try {
        m_windowThread = std::thread(&D2DWindow::WindowThread, this, m_windowTitle.c_str(), m_width, m_height);
    } catch (const std::exception&) {
        m_isRunning = false;
        throw;
    }
}

void D2DWindow::Stop() {
    if (m_windowThread.joinable()) {
        DestroyWindow(m_hwnd);
        m_isRunning = false;
        m_windowThread.join();
    }
}

HRESULT D2DWindow::RegisterWindowClass(HINSTANCE hInstance) {
    WNDCLASSEX wc = { sizeof(WNDCLASSEX) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = StaticWndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = m_className.c_str();

    if (!RegisterClassEx(&wc)) {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    return S_OK;
}

LRESULT CALLBACK D2DWindow::StaticWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    D2DWindow* pThis = nullptr;

    if (message == WM_NCCREATE) {
        pThis = static_cast<D2DWindow*>(reinterpret_cast<LPCREATESTRUCT>(lParam)->lpCreateParams);
        SetWindowLongPtr(hWnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pThis));
    } else {
        pThis = reinterpret_cast<D2DWindow*>(GetWindowLongPtr(hWnd, GWLP_USERDATA));
    }

    if (pThis) {
        return pThis->WndProc(hWnd, message, wParam, lParam);
    }

    return DefWindowProc(hWnd, message, wParam, lParam);
}

LRESULT CALLBACK D2DWindow::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_DESTROY:
            m_isRunning = false;
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
}