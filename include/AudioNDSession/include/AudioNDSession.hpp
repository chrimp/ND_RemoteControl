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

namespace AudioNDSession {
    constexpr size_t MINIMUM_PERIOD = 10; // WASAPI shared 10ms
    constexpr size_t SAMPLE_RATE = 48000; // 48kHz
    constexpr size_t CHANNELS = 2; // Stereo
    constexpr size_t BYTES_PER_SAMPLE = 4; // 32-bit audio (for testing purposes, target int16)
    constexpr size_t AUDIO_BUFFER_SIZE = SAMPLE_RATE * CHANNELS * BYTES_PER_SAMPLE / 100; // 10ms buffer
    constexpr size_t FRAMES_PER_PACKET = AUDIO_BUFFER_SIZE / (CHANNELS * BYTES_PER_SAMPLE);
    constexpr size_t FLAG_LENGTH = 1;
    constexpr size_t QP_CHECK_RESERVE_LENGTH = 1;
    constexpr size_t BUFFER_ALLOC_SIZE = FLAG_LENGTH + AUDIO_BUFFER_SIZE + QP_CHECK_RESERVE_LENGTH;

    class AudioNDSessionServer : private NDSessionServerBase {
        public:
        bool Setup(char* localAddr);
        void OpenListener(const char* localAddr, const unsigned short port);
        void ExchangePeerInfo();

        private:
        void Loop();
        bool WaitForCompletionAndCheckContext(void *expectedContext, ULONG notifyFlag = ND_CQ_NOTIFY_ANY);
        bool WaitForCompletionAndCheckContextWithTimeout(void *expectedContext, bool& didTimedOut, ULONG notifyFlag = ND_CQ_NOTIFY_ANY);

        public:
        void Start(char* localAddr, const unsigned short port) {
            Setup(localAddr);
            OpenListener(localAddr, port);
            ExchangePeerInfo();

            m_isRunning = true;
            m_thread = std::thread(&AudioNDSessionServer::Loop, this);
        }

        void Stop() {
            if (m_hEvent) {
                SetEvent(m_hEvent);
                CloseHandle(m_hEvent);
                m_hEvent = nullptr;
            }
            m_isRunning = false;
            Shutdown();
            if (m_thread.joinable()) {
                m_thread.join();
            }
        }

        private:
        PeerInfo remoteInfo;
        std::atomic<bool> m_isRunning = true;
        std::thread m_thread;
        std::array<unsigned short, AUDIO_BUFFER_SIZE> m_audioData;

        bool m_isTest = false;
        HANDLE m_hEvent = NULL;
    };

    class AudioNDSessionClient : private NDSessionClientBase {
        public:
        AudioNDSessionClient() {}

        bool Setup(const char* localAddr);
        void OpenConnector(const char* localAddr, const char* serverAddr, const unsigned short port);
        void ExchangePeerInfo();

        private:
        void Loop();

        bool WaitForCompletionAndCheckContext(void *expectedContext, ULONG notifyFlag = ND_CQ_NOTIFY_ANY);
        bool WaitForCompletionAndCheckContextWithTimeout(void *expectedContext, bool& didTimedOut, ULONG notifyFlag = ND_CQ_NOTIFY_ANY);
        
        public:
        void Start(char* localAddr, const char* serverAddr, const unsigned short port) {
            Setup(localAddr);
            OpenConnector(localAddr, serverAddr, port);
            ExchangePeerInfo();

            m_isRunning = true;
            m_thread = std::thread(&AudioNDSessionClient::Loop, this);
        }
        void Stop() {
            if (m_hEvent) {
                SetEvent(m_hEvent);
                CloseHandle(m_hEvent);
                m_hEvent = nullptr;
            }
            Shutdown();
            m_isRunning = false;
            if (m_thread.joinable()) {
                m_thread.join();
            }
        }

        void EnableTestMode() {
            m_isTest = true;
        }

        void GetEventHandle(HANDLE& hEvent) {
            if (!m_isTest) return;
            hEvent = m_hEvent;
            return;
        }

        private:
        std::atomic<bool> m_isRunning = true;
        PeerInfo remoteInfo;
        std::thread m_thread;
        ComPtr<IAudioCaptureClient> m_audioCaptureClient;

        bool m_isTest = false;
        HANDLE m_hEvent = NULL;
    };
}

#endif