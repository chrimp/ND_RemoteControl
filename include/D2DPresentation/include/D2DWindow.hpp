#ifndef D2DWINDOW_H
#define D2DWINDOW_H
#pragma once

#include <Windows.h>
#include <thread>
#include <atomic>

#include "tstring.hpp"

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

namespace D2DPresentation {
    class D2DWindow {
    public:
        D2DWindow(
            LPCTSTR title,
            UINT width,
            UINT height
        );
        ~D2DWindow();

        void Start();
        void Stop();

        HWND GetHwnd() const { return m_hwnd; }

    private:
        void WindowThread(
            LPCTSTR title,
            UINT width,
            UINT height
        );

        static LRESULT CALLBACK StaticWndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);
        LRESULT CALLBACK WndProc(HWND hWnd, UINT message, WPARAM wParam, LPARAM lParam);

        HRESULT RegisterWindowClass(HINSTANCE hInstance);

        HWND m_hwnd = NULL;
        HINSTANCE m_hInstance = NULL;

        tstring m_windowTitle = "D2D Window";
        tstring m_className = "D2DWindowClass";
        
        std::thread m_windowThread;
        std::atomic<bool> m_isRunning = false;

        UINT m_width = 800;
        UINT m_height = 600;
    };
}

#endif