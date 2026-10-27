#ifndef NDSESSION_HPP
#define NDSESSION_HPP
#include "ndspi.h"
#include <minwinbase.h>
#pragma once

#include <WinSock2.h>
#include <WS2tcpip.h>
#include <ndsupport.h>
#include <variant>
#include <iostream>

#define RECV_CTXT ((void*)0x1000)
#define SEND_CTXT ((void*)0x2000)
#define READ_CTXT ((void*)0x3000)
#define WRITE_CTXT ((void*)0x4000)

struct PeerInfo {
    UINT64 remoteAddr;
    UINT32 remoteToken;
};

class NDSessionBase {
    public:
    IND2QueuePair* GetQP() { return m_pQp; }

    protected:
    IND2Adapter *m_pAdapter = nullptr;
    IND2MemoryRegion *m_pMr = nullptr;
    IND2CompletionQueue* m_pCq = nullptr;

    IND2CompletionQueue *m_pReceiveCq = nullptr;
    IND2CompletionQueue *m_pInitiatorCq = nullptr;

    IND2QueuePair *m_pQp = nullptr;
    IND2Connector *m_pConnector = nullptr;
    HANDLE m_hAdapterFile = nullptr;
    DWORD m_Buf_Len = 0;
    void* m_Buf = nullptr;
    IND2MemoryWindow *m_pMw = nullptr;
    OVERLAPPED m_Ov{};

    OVERLAPPED m_ReceiveOv{};
    OVERLAPPED m_InitiatorOv{};

    size_t m_MaxPerTransfer = 1500;

    protected:
    NDSessionBase();
    ~NDSessionBase();

    // Initialize the adapter with the given ipv4 addr
    bool Initialize(char* localAddr);

    HRESULT CreateMW();
    HRESULT InvalidateMW();
    
    ND2_ADAPTER_INFO GetAdapterInfo();

    HRESULT CreateMR();
    HRESULT RegisterDataBuffer(DWORD bufferLength, ULONG type);
    HRESULT RegisterDataBuffer(void *pBuffer, DWORD bufferLength, ULONG type);
    [[deprecated("Temporarily disabled")]]
    HRESULT CreateCQ(DWORD depth);
    HRESULT CreateCQ(IND2CompletionQueue **ppCq, DWORD depth);
    HRESULT CreateConnector();
    HRESULT CreateQP(DWORD queueDepth, DWORD nSge, DWORD inlineDataSize = 0);
    HRESULT CreateQP(DWORD receiveQueueDepth, DWORD initiatorQueueDepth, DWORD maxReceiveRequestSge, DWORD maxInitiatorRequestSge);
    HRESULT CreateQP(DWORD receiveQueueDepth, DWORD initiatorQueueDepth, DWORD maxReceiveRequestSge, DWORD maxInitiatorRequestSge, DWORD maxInlineDataSize);

    //void ClearOPs();
    
    void DisconnectConnector();
    void DeregisterMemory();

    HRESULT GetResult(IND2CompletionQueue *pCq, OVERLAPPED* pOv);

    DWORD PrepareSge(ND2_SGE *pSge, const DWORD nSge, char* pBuf, ULONG buffSize, ULONG headerSize, UINT32 memoryToken);

    HRESULT PostReceive(const ND2_SGE* Sge, const DWORD nSge, void *requestContext = nullptr);

    HRESULT Send(const ND2_SGE* Sge, const ULONG nSge, ULONG flags, void* requestContext = nullptr);
    HRESULT Write(const ND2_SGE* Sge, const ULONG nSge, UINT64 remoteAddr, UINT32 remoteToken, DWORD flags, void *requestContext = nullptr);
    HRESULT Read(const ND2_SGE* Sge, const ULONG nSge, UINT64 remoteAddr, UINT32 remoteToken, DWORD flags, void *requestContext = nullptr);

    [[deprecated("Temporarily disabled")]]
    void WaitForEventNotification(ULONG notifyFlag);
    void WaitForEventNotification(IND2CompletionQueue* pCq, OVERLAPPED* pOv, ULONG notifyFlag);
    [[deprecated("Temporarily disabled")]]
    ND2_RESULT WaitForCompletion(ULONG notifyFlag, bool bBlocking = true);
    ND2_RESULT WaitForCompletion(IND2CompletionQueue* pCq, OVERLAPPED* pOv, ULONG notifyFlag, bool bBlocking = true);
    HRESULT WaitForCompletion();
    bool WaitForCompletionAndCheckContext(void *expectedContext, ULONG notifyFlag = ND_CQ_NOTIFY_ANY);

    bool WaitForEventNotificationWithTimeout(IND2CompletionQueue* pCq, OVERLAPPED* pOv, ULONG notifyFlag, DWORD timeoutMs);
    ND2_RESULT WaitForCompletionWithTimeout(IND2CompletionQueue* pCq, OVERLAPPED* pOv, ULONG notifyFlag, DWORD timeoutMs, bool& didTimedOut);

    std::variant<HRESULT, ND2_RESULT> Bind(DWORD bufferLength, ULONG type, void *context = nullptr);
    std::variant<HRESULT, ND2_RESULT> Bind(const void *pBuf, DWORD BufferLength, ULONG type, void *context = nullptr);

    void Shutdown();

    HRESULT FlushQP();

    HRESULT Reject(const VOID *pPrivateData, DWORD cbPrivateData);
};

class NDSessionServerBase : public NDSessionBase {
    protected:
    IND2Listener *m_pListen;

    public:
    NDSessionServerBase();
    ~NDSessionServerBase();

    HRESULT CreateListener();
    HRESULT Listen(const char *localAddr);
    HRESULT GetConnectionRequest();
    HRESULT Accept(DWORD inboundReadLimit, DWORD outboundReadLimit, const void *pPrivateData, DWORD cbPrivateData);
};

class NDSessionClientBase : public NDSessionBase {
    public:
    HRESULT Connect(const char *localAddr, const char *remoteAddr, DWORD inboundReadLimit, DWORD outboundReadLimit, const void *pPrivateData = nullptr, DWORD cbPrivateData = 0);
    HRESULT CompleteConnect();
};

#endif // NDSESSION_HPP