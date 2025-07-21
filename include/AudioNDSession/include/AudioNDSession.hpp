#ifndef AUDIONDSESSION_HPP
#define AUDIONDSESSION_HPP

#pragma once

#include "NDSession.hpp"
#include <array>
#include <thread>
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <audiopolicy.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

constexpr size_t MINIMUM_PERIOD = 10; // WASAPI shared 10ms
constexpr size_t SAMPLE_RATE = 44100; // 44.1kHz
constexpr size_t CHANNELS = 2; // Stereo
constexpr size_t BYTES_PER_SAMPLE = 2; // 16-bit audio
constexpr size_t AUDIO_BUFFER_SIZE = SAMPLE_RATE * CHANNELS * BYTES_PER_SAMPLE / 100; // 10ms buffer
constexpr size_t BUFFER_ALLOC_SIZE = 1 + AUDIO_BUFFER_SIZE;

class AudioNDSessionServer : private NDSessionServerBase {
    public:
    bool Setup(char* localAddr);
    void OpenListener(const char* localAddr);
    void ExchangePeerInfo();

    private:
    void Loop();

    public:
    void Start(char* localAddr) {
        m_hCallbackEvent = CreateEvent(NULL, FALSE, FALSE, NULL);
        Setup(localAddr);
        OpenListener(localAddr);
        ExchangePeerInfo();

        m_isRunning = true;
        m_thread = std::thread(&AudioNDSessionServer::Loop, this);
    }

    void Stop() {
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
    std::array<unsigned short, AUDIO_BUFFER_SIZE> m_audioData;

    ComPtr<IAudioCaptureClient> m_audioCaptureClient;

    HANDLE m_hCallbackEvent = nullptr;

    bool WaitForCompletionAndCheckContext(void *expectedContext, ULONG notifyFlag = ND_CQ_NOTIFY_ANY);
};

class AudioNDSessionClient : private NDSessionClientBase {
    public:
    AudioNDSessionClient() {}

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
        m_thread = std::thread(&AudioNDSessionClient::Loop, this);
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

    bool WaitForCompletionAndCheckContext(void *expectedContext, ULONG notifyFlag = ND_CQ_NOTIFY_ANY);
};

#endif