#ifndef INPUTNDSESSION_HPP
#define INPUTNDSESSION_HPP

#pragma once

#include "NDSession.hpp"
#include <thread>
#include <mutex>

constexpr size_t INPUT_EVENT_BUFFER_SIZE = 512;

class InputNDSessionServer : private NDSessionServerBase {
    struct Move {
        int dx;
        int dy;
    };
    struct AtomicMouse {
        std::atomic<short> x;
        std::atomic<short> y;
        std::atomic<short> wheel;
        std::atomic<bool> absolute;
        std::atomic<ULONG> buttonFlags;
    };

    struct AtomicKeyboard {
        std::atomic<unsigned short> scanCode;
        std::atomic<unsigned char> down; // 0 - down, 1 - up, 2 - nothing
        std::atomic<bool> isE0;
    };

    struct MousePacket {
        short x;
        short y;
        short wheel;
        bool absolute;
        ULONG buttonFlags;
    };

    struct KeyPacket {
        unsigned short scanCode;
        unsigned char down; // 0 - down, 1 - up, 2 - nothing
        bool isE0;
    };

    struct Packet {
        MousePacket mouse;
        KeyPacket key;
    };

    struct Point {
        int x;
        int y;
    };

    public:
    bool Setup(char* localAddr);
    void OpenListener(const char* localAddr, const unsigned short port);
    void ExchangePeerInfo();
    void SendEvent(RAWINPUT input);

    private:
    void Loop();

    public:
    void Start(char* localAddr, const unsigned short port) {
        m_hCallbackEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        Setup(localAddr);
        OpenListener(localAddr, port);
        ExchangePeerInfo();

        m_isRunning = true;
        m_thread = std::thread(&InputNDSessionServer::Loop, this);
    }

    void Stop() {
        if (m_hCallbackEvent) {
            SetEvent(m_hCallbackEvent);
            CloseHandle(m_hCallbackEvent);
            m_hCallbackEvent = nullptr;
        }
        Shutdown();
        m_isRunning = false;
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    HANDLE GetCallbackEvent() const {
        return m_hCallbackEvent;
    }

    private:
    PeerInfo remoteInfo;
    std::atomic<bool> m_isRunning = true;
    std::thread m_thread;
    std::atomic<unsigned long long> m_CurSge = 0;
    unsigned char m_MaxSge = 0;

    std::mutex m_SendMutex;

    std::atomic<bool> m_working = false;
    AtomicMouse m_Mouse = {0, 0, 0, 0};
    AtomicKeyboard m_Keyboard = {0, 2}; // 2 - nothing

    HANDLE m_hCallbackEvent = nullptr;

    bool WaitForCompletionAndCheckContext(void *expectedContext, ULONG notifyFlag = ND_CQ_NOTIFY_ANY);
};

class InputNDSessionClient : private NDSessionClientBase {
    struct MousePacket {
        short x;
        short y;
        short wheel;
        bool absolute;
        ULONG buttonFlags;
    };
    struct KeyPacket {
        unsigned short scanCode;
        unsigned char down; // 0 - down, 1 - up, 2 - nothing
        bool isE0;
    };

    struct Packet {
        MousePacket mouse;
        KeyPacket key;
    };

    public:
    InputNDSessionClient(std::mutex& coutMutex) : m_coutMutex(coutMutex) {}

    bool Setup(const char* localAddr);
    void OpenConnector(const char* localAddr, const char* serverAddr, const unsigned short port);
    void ExchangePeerInfo();

    private:
    void Loop();
    bool WaitForCompletionAndCheckContext(void *expectedContext, ULONG notifyFlag = ND_CQ_NOTIFY_ANY);
    
    public:
    void Start(char* localAddr, const char* serverAddr, const unsigned short port) {
        Setup(localAddr);
        OpenConnector(localAddr, serverAddr, port);
        ExchangePeerInfo();

        m_isRunning = true;
        m_thread = std::thread(&InputNDSessionClient::Loop, this);
    }
    void Stop() {
        Shutdown();
        m_isRunning = false;
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    void EnableTestMode() {
        m_isTest = true;
    }

    bool GetMouse(short& x, short& y) const {
        if (!m_isTest) return false;
        x = m_Packet.mouse.x;
        y = m_Packet.mouse.y;
        return true;
    }

    void SetEventHandle(HANDLE hEvent) {
        if (!m_isTest) return;
        m_hEvent = hEvent;
        return;
    }

    private:
    std::atomic<bool> m_isRunning = true;
    PeerInfo remoteInfo;
    std::thread m_thread;

    std::mutex& m_coutMutex;

    bool m_isTest = false;

    Packet m_Packet = { {0, 0, 0, false, 0}, {0, 2, false} };
    HANDLE m_hEvent = NULL;
};

#endif