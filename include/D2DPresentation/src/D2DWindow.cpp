#include "../include/D2DWindow.hpp"
#include <minwindef.h>
#include <objbase.h>
#include <windowsx.h>
#include <hidusage.h>
#include <iostream>
#include <dwmapi.h>
#define ABSCURSOR

#pragma comment(lib, "dwmapi.lib")

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

extern std::atomic<bool> g_shouldQuit;

using namespace D2DPresentation;

D2DWindow::D2DWindow(
    LPCTSTR title,
    UINT width,
    UINT height
) {
    m_windowTitle = title;
    m_width = width;
    m_height = height;
    m_aspectRatio = static_cast<float>(width) / static_cast<float>(height);
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

    RECT rect = { 0, 0, static_cast<LONG>(width), static_cast<LONG>(height) };
    AdjustWindowRect(&rect, WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);

    m_hwnd = CreateWindowEx(
        0,
        m_className.c_str(),
        title,
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
        NULL, NULL, hInstance, this
    );

    if (!m_hwnd) {
        hr = HRESULT_FROM_WIN32(GetLastError());
        throw std::exception();
    }

    RegisterRawInput();

    // WH_KEYBOARD_LL hook cannot work with a process that registers raw input devices.
    // So it should either use RIDEV_NOHOTKEYS | RIDEV_NOLEGACY | RIDEV_APPKEYS or create another process for the hook,
    // if I want to keep using raw input (or low-level keyboard hook).
    // So let's just give up alt-tab blocking for now.

    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(m_hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &darkMode, sizeof(darkMode));
    DWM_WINDOW_CORNER_PREFERENCE cornerPreference = DWM_WINDOW_CORNER_PREFERENCE::DWMWCP_DEFAULT;
    DwmSetWindowAttribute(m_hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPreference, sizeof(cornerPreference));

    while (!m_show) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    ShowWindow(m_hwnd, SW_SHOWDEFAULT);
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

void D2DWindow::RegisterRawInput() {
    RAWINPUTDEVICE rid[2];
    // Mouse
    rid[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
    rid[0].usUsage = HID_USAGE_GENERIC_MOUSE;
    rid[0].dwFlags = 0;
    rid[0].hwndTarget = m_hwnd;

    // Keyboard
    rid[1].usUsagePage = HID_USAGE_PAGE_GENERIC;
    rid[1].usUsage = HID_USAGE_GENERIC_KEYBOARD;
    rid[1].dwFlags = RIDEV_NOLEGACY | RIDEV_NOHOTKEYS | RIDEV_APPKEYS;
    rid[1].hwndTarget = m_hwnd;

    if (!RegisterRawInputDevices(rid, 2, sizeof(RAWINPUTDEVICE))) {
        std::cerr << "Failed to register raw input devices: " << std::hex << GetLastError() << std::endl;
        throw std::exception();
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
        //DestroyWindow(m_hwnd);
        PostMessage(m_hwnd, WM_CLOSE, 0, 0);

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

static bool bDoNotTrap = true;

LRESULT CALLBACK D2DWindow::WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_NCLBUTTONDOWN:
            // Prevent cursor trap
            bDoNotTrap = true;
            ShowCursor(true);
            break;
        case WM_CLOSE:
            if (m_cursorTrapped) {
                ClipCursor(nullptr);
                ShowCursor(true);
                m_cursorTrapped = false;
            }
            g_shouldQuit.store(true);
            DestroyWindow(hWnd);

            if (m_rawInputCallback) {
                SetEvent(m_hCallbackEvent);
            }
            return 0;
        case WM_DESTROY:
            m_isRunning = false;
            PostQuitMessage(0);
            return 0;
        case WM_INPUT: {
            if (g_shouldQuit.load()) {
                PostMessage(hWnd, WM_CLOSE, 0, 0);
                return 0;
            }
            UINT dwSize = 0;
            UINT res = GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, nullptr, &dwSize, sizeof(RAWINPUTHEADER));
            if (res == -1) {
                volatile LONG error = GetLastError();
                throw std::exception();
            }

            std::vector<BYTE> lpb(dwSize);

            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, lpb.data(), &dwSize, sizeof(RAWINPUTHEADER)) != dwSize) {
                std::cerr << "Failed to get raw input data: " << std::hex << GetLastError() << std::endl;
                throw std::exception();
            }
            RAWINPUT* rawInput = reinterpret_cast<RAWINPUT*>(lpb.data());

            #ifdef ABSCURSOR
            if (rawInput->header.dwType == RIM_TYPEMOUSE) {
                POINT pos;
                bool success = GetCursorPos(&pos);

                if (!success) {
                    volatile LONG error = GetLastError();
                    throw std::exception();
                }

                success = ScreenToClient(hWnd, &pos);
                if (!success) {
                    volatile LONG error = GetLastError();
                    throw std::exception();
                }

                rawInput->data.mouse.lLastX = pos.x;
                rawInput->data.mouse.lLastY = pos.y;
                rawInput->data.mouse.usFlags |= MOUSE_MOVE_ABSOLUTE;
            }
            #endif

            if (m_rawInputCallback) {
                m_rawInputCallback(*rawInput);
                SetEvent(m_hCallbackEvent);
            } else {
                bDoNotTrap = true;
            }
            break;
        }
        case WM_LBUTTONDOWN:
            #ifndef ABSCURSOR
            bDoNotTrap = false;
            #endif
            while(ShowCursor(false) >= 0);
            // fallthrough        
        case WM_SETFOCUS:
            if (bDoNotTrap) return 0;
            while(ShowCursor(false) >= 0);
            {
                RECT rect;
                GetClientRect(hWnd, &rect);
                ClientToScreen(hWnd, reinterpret_cast<POINT*>(&rect));
                ClientToScreen(hWnd, reinterpret_cast<POINT*>(&rect) + 1);
                ClipCursor(&rect);
                m_cursorTrapped = true;
            }
            return 0;
        case WM_KILLFOCUS:
            ClipCursor(nullptr);
            ShowCursor(true);
            m_cursorTrapped = false;
            return 0;
        case WM_SIZING:
        {
            RECT* pRect = reinterpret_cast<RECT*>(lParam);
            float sourceAspect = m_aspectRatio;

            DWORD style = GetWindowLong(hWnd, GWL_STYLE);
            DWORD exStyle = GetWindowLong(hWnd, GWL_EXSTYLE);

            // Get proposed window dimensions
            int windowWidth = pRect->right - pRect->left;
            int windowHeight = pRect->bottom - pRect->top;
            
            // Calculate border sizes once
            RECT borderRect = { 0, 0, 0, 0 };
            AdjustWindowRectEx(&borderRect, style, FALSE, exStyle);
            int borderWidth = (borderRect.right - borderRect.left);
            int borderHeight = (borderRect.bottom - borderRect.top);
            
            // Extract client dimensions
            int clientWidth = windowWidth - borderWidth;
            int clientHeight = windowHeight - borderHeight;

            int edge = static_cast<int>(wParam);
            
            // Calculate new client dimensions
            int newClientWidth, newClientHeight;
            
            if (edge == WMSZ_LEFT || edge == WMSZ_RIGHT) {
                // Width is primary
                newClientWidth = clientWidth;
                newClientHeight = static_cast<int>(clientWidth / sourceAspect + 0.5f); // Round to nearest
            } else if (edge == WMSZ_TOP || edge == WMSZ_BOTTOM) {
                // Height is primary
                newClientHeight = clientHeight;
                newClientWidth = static_cast<int>(clientHeight * sourceAspect + 0.5f); // Round to nearest
            } else {
                // Corner - prefer the larger change to avoid jitter
                float currentAspect = static_cast<float>(clientWidth) / static_cast<float>(clientHeight);
                
                if (currentAspect > sourceAspect) {
                    // Current is too wide - constrain by height
                    newClientHeight = clientHeight;
                    newClientWidth = static_cast<int>(clientHeight * sourceAspect + 0.5f);
                } else {
                    // Current is too tall - constrain by width
                    newClientWidth = clientWidth;
                    newClientHeight = static_cast<int>(clientWidth / sourceAspect + 0.5f);
                }
            }
            
            // Convert back to window dimensions
            int newWindowWidth = newClientWidth + borderWidth;
            int newWindowHeight = newClientHeight + borderHeight;
            
            // Apply new dimensions with proper anchoring
            switch (edge) {
                case WMSZ_LEFT:
                    pRect->left = pRect->right - newWindowWidth;
                    pRect->bottom = pRect->top + newWindowHeight;
                    break;
                case WMSZ_RIGHT:
                    pRect->right = pRect->left + newWindowWidth;
                    pRect->bottom = pRect->top + newWindowHeight;
                    break;
                case WMSZ_TOP:
                    pRect->top = pRect->bottom - newWindowHeight;
                    pRect->right = pRect->left + newWindowWidth;
                    break;
                case WMSZ_BOTTOM:
                    pRect->bottom = pRect->top + newWindowHeight;
                    pRect->right = pRect->left + newWindowWidth;
                    break;
                case WMSZ_TOPLEFT:
                    pRect->left = pRect->right - newWindowWidth;
                    pRect->top = pRect->bottom - newWindowHeight;
                    break;
                case WMSZ_TOPRIGHT:
                    pRect->right = pRect->left + newWindowWidth;
                    pRect->top = pRect->bottom - newWindowHeight;
                    break;
                case WMSZ_BOTTOMLEFT:
                    pRect->left = pRect->right - newWindowWidth;
                    pRect->bottom = pRect->top + newWindowHeight;
                    break;
                case WMSZ_BOTTOMRIGHT:
                    pRect->right = pRect->left + newWindowWidth;
                    pRect->bottom = pRect->top + newWindowHeight;
                    break;
            }

            return TRUE;
        }
        default:
            return DefWindowProc(hWnd, message, wParam, lParam);
    }
    return DefWindowProc(hWnd, message, wParam, lParam);
}