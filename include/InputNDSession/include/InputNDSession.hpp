#ifndef INPUTNDSESSION_HPP
#define INPUTNDSESSION_HPP

#pragma once

#include "NDSession.hpp"
#include <chrono>
#include <thread>
#include <mutex>

constexpr size_t INPUT_EVENT_BUFFER_SIZE = 1024;

class InputNDSessionServer : private NDSessionServerBase {
    public:
    bool Setup(char* localAddr);

    void OpenListener(const char* localAddr);

    void ExchangePeerInfo();

    private:
    void Loop();

    public:
    void Start(char* localAddr) {
        Setup(localAddr);
        OpenListener(localAddr);
        ExchangePeerInfo();

        m_isRunning = true;
        m_thread = std::thread(&InputNDSessionServer::Loop, this);
    }

    void Stop() {
        m_isRunning = false;
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    private:
    PeerInfo remoteInfo;
    std::atomic<bool> m_isRunning = true;
    std::thread m_thread;

    bool WaitForCompletionAndCheckContext(void *expectedContext, ULONG notifyFlag = ND_CQ_NOTIFY_ANY);
};

class InputNDSessionClient : private NDSessionClientBase {
    public:
    InputNDSessionClient(std::mutex& coutMutex) : m_coutMutex(coutMutex) {}

    bool Setup(const char* localAddr);
    void OpenConnector(const char* localAddr, const char* serverAddr);
    void ExchangePeerInfo();

    private:
    void Loop();
    
    public:
    void Start(char* localAddr, const char* serverAddr) {
        Setup(localAddr);
        OpenConnector(localAddr, serverAddr);
        ExchangePeerInfo();

        m_isRunning = true;
        m_thread = std::thread(&InputNDSessionClient::Loop, this);
    }
    void Stop() {
        m_isRunning = false;
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    private:
    std::atomic<bool> m_isRunning = true;
    PeerInfo remoteInfo;
    std::thread m_thread;

    std::mutex& m_coutMutex;

    bool WaitForCompletionAndCheckContext(void *expectedContext, ULONG notifyFlag = ND_CQ_NOTIFY_ANY);
};

#endif