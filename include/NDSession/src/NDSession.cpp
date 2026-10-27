#include "NDSession.hpp"
#include "ndspi.h"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <minwinbase.h>
#include <stdexcept>


template<typename T>
void SafeRelease(T*& p) {
    if (p != nullptr) {
        p->Release();
        p = nullptr;
    }
}

// MARK: NDSessionBase
NDSessionBase::NDSessionBase() {
    //RtlZeroMemory(&m_ReceiveOv, sizeof(m_ReceiveOv));
    //RtlZeroMemory(&m_InitiatorOv, sizeof(m_InitiatorOv));
}

NDSessionBase::~NDSessionBase() {
    SafeRelease(m_pMr);
    SafeRelease(m_pMw);
    SafeRelease(m_pReceiveCq);
    SafeRelease(m_pInitiatorCq);
    SafeRelease(m_pQp);
    SafeRelease(m_pConnector);
    if (m_hAdapterFile) CloseHandle(m_hAdapterFile);
    SafeRelease(m_pAdapter);
    if (m_Buf) {
        VirtualFree(m_Buf, 0, MEM_RELEASE);
        m_Buf = nullptr;
    }
}

HRESULT NDSessionBase::CreateMR() {
    HRESULT hr = m_pAdapter->CreateMemoryRegion(IID_IND2MemoryRegion, m_hAdapterFile, reinterpret_cast<void**>(&m_pMr));
    return hr;
}

HRESULT NDSessionBase::RegisterDataBuffer(DWORD bufferLength, ULONG type) {
    if (m_Buf) {
        HRESULT hr = m_pMr->Deregister(&m_InitiatorOv);
        if (hr == ND_PENDING) {
            hr = m_pMr->GetOverlappedResult(&m_InitiatorOv, true);
        } else if (FAILED(hr)) {
            std::cerr << "Failed to deregister memory region: " << std::hex << hr << std::endl;
            #ifdef _DEBUG
            //abort();
            #endif
            return hr;
        }
        VirtualFree(m_Buf, 0, MEM_RELEASE);
        m_Buf = nullptr;
    }

    m_Buf_Len = bufferLength;
    m_Buf = VirtualAlloc(nullptr, m_Buf_Len, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!m_Buf) {
        std::cerr << "Failed to allocate memory for buffer." << std::endl;
        return E_OUTOFMEMORY;
    }

    return RegisterDataBuffer(m_Buf, m_Buf_Len, type);
}

HRESULT NDSessionBase::RegisterDataBuffer(void *pBuf, DWORD bufferLength, ULONG type) {
    HRESULT hr = m_pMr->Register(pBuf, bufferLength, type, &m_InitiatorOv);
    if (hr == ND_PENDING) {
        hr = m_pMr->GetOverlappedResult(&m_InitiatorOv, true);
    }
    return hr;
}

HRESULT NDSessionBase::CreateMW() {
    HRESULT hr = m_pAdapter->CreateMemoryWindow(IID_IND2MemoryWindow, reinterpret_cast<void**>(&m_pMw));
    return hr;
}

HRESULT NDSessionBase::InvalidateMW() {
    HRESULT hr = m_pQp->Invalidate(nullptr, m_pMw, 0);
    return hr;
}

std::variant<HRESULT, ND2_RESULT> NDSessionBase::Bind(DWORD bufferLength, ULONG flags, void *context) {
    return Bind(m_Buf, bufferLength, flags, context);
}

std::variant<HRESULT, ND2_RESULT> NDSessionBase::Bind(const void *pBuf, DWORD bufferLength, ULONG flags, void *context) {
    HRESULT hr = m_pQp->Bind(context, m_pMr, m_pMw, pBuf, bufferLength, flags);
    if (hr != ND_SUCCESS) {
        return hr;
    }

    ND2_RESULT ndRes = WaitForCompletion(m_pInitiatorCq, &m_InitiatorOv, ND_CQ_NOTIFY_ANY, true);
    if (ndRes.Status == ND_SUCCESS && ndRes.RequestContext != context) {
        return E_INVALIDARG;
    }

    return ndRes;
}

HRESULT NDSessionBase::CreateCQ(DWORD depth) {
    HRESULT hr = m_pAdapter->CreateCompletionQueue(IID_IND2CompletionQueue, m_hAdapterFile, depth, 0, 0, reinterpret_cast<void**>(&m_pReceiveCq));
    return hr;
}

HRESULT NDSessionBase::CreateCQ(IND2CompletionQueue **ppCq, DWORD depth) {
    depth = 128;
    HRESULT hr = m_pAdapter->CreateCompletionQueue(IID_IND2CompletionQueue, m_hAdapterFile, depth, 0, 0, reinterpret_cast<void**>(ppCq));
    return hr;
}

HRESULT NDSessionBase::CreateConnector() {
    HRESULT hr = m_pAdapter->CreateConnector(IID_IND2Connector, m_hAdapterFile, reinterpret_cast<void**>(&m_pConnector));
    return hr;
}

HRESULT NDSessionBase::CreateQP(DWORD queueDepth, DWORD nSge, DWORD inlineDataSize) {
    HRESULT hr = m_pAdapter->CreateQueuePair(IID_IND2QueuePair, m_pReceiveCq, m_pInitiatorCq, nullptr, queueDepth, queueDepth,
        nSge, nSge, inlineDataSize, reinterpret_cast<void**>(&m_pQp));
    return hr;
}

HRESULT NDSessionBase::CreateQP(DWORD receiveQueueDepth, DWORD initiatorQueueDepth, DWORD maxReceiveRequestSge, DWORD maxInitiatorRequestSge) {
    receiveQueueDepth = std::clamp(receiveQueueDepth, (unsigned long)1, (unsigned long)128);
    initiatorQueueDepth = std::clamp(initiatorQueueDepth, (unsigned long)1, (unsigned long)128);

    HRESULT hr = m_pAdapter->CreateQueuePair(IID_IND2QueuePair, m_pReceiveCq, m_pInitiatorCq, nullptr, receiveQueueDepth, initiatorQueueDepth,
        maxReceiveRequestSge, maxInitiatorRequestSge, 0, reinterpret_cast<void**>(&m_pQp));
    return hr;
}

HRESULT NDSessionBase::CreateQP(DWORD receiveQueueDepth, DWORD initiatorQueueDepth, DWORD maxReceiveRequestSge, DWORD maxInitiatorRequestSge, DWORD maxInlineDataSize) {
    receiveQueueDepth = std::clamp(receiveQueueDepth, (unsigned long)1, (unsigned long)128);
    initiatorQueueDepth = std::clamp(initiatorQueueDepth, (unsigned long)1, (unsigned long)128);

    HRESULT hr = m_pAdapter->CreateQueuePair(IID_IND2QueuePair, m_pReceiveCq, m_pInitiatorCq, nullptr, receiveQueueDepth, initiatorQueueDepth,
        maxReceiveRequestSge, maxInitiatorRequestSge, maxInlineDataSize, reinterpret_cast<void**>(&m_pQp));
    return hr;
}

bool NDSessionBase::Initialize(char* localAddr) {
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    int len = sizeof(addr);
    WSAStringToAddress(localAddr, AF_INET, nullptr, reinterpret_cast<struct sockaddr*>(&addr), &len);

    HRESULT hr = NdOpenAdapter(IID_IND2Adapter, reinterpret_cast<const struct sockaddr*>(&addr), sizeof(addr), reinterpret_cast<void**>(&m_pAdapter));
        if (hr == ND_INVALID_ADDRESS) {
        std::cerr << "ND_INVALID_ADDRESS: The specified IP does not correspond to an RDMA-capable adapter for NDv2." << std::endl;
        return false;
    }

    if (FAILED(hr)) {
        std::cerr << "Failed to open adapter: " << std::hex << hr << std::endl;
        return false;
    }

    ND2_ADAPTER_INFO info = { 0 };
    info.InfoVersion = ND_VERSION_2;
    ULONG adapterInfoSize = sizeof(info);
    hr = m_pAdapter->Query(&info, &adapterInfoSize);
    if (FAILED(hr)) {
        std::cerr << "Failed to query adapter info: " << std::hex << hr << std::endl;
        SafeRelease(m_pAdapter);
        return false;
    }

    m_MaxPerTransfer = info.MaxTransferLength;

    m_ReceiveOv.hEvent = CreateEvent(nullptr, false, false, nullptr);
    if (m_ReceiveOv.hEvent == nullptr) {
        std::cerr << "Failed to create event for overlapped operations." << std::endl;
        return false;
    }
    m_InitiatorOv.hEvent = CreateEvent(nullptr, false, false, nullptr);
    if (m_InitiatorOv.hEvent == nullptr) {
        std::cerr << "Failed to create event for overlapped operations." << std::endl;
        CloseHandle(m_ReceiveOv.hEvent);
        return false;
    }

    hr = m_pAdapter->CreateOverlappedFile(&m_hAdapterFile);
    if (FAILED(hr)) {
        std::cerr << "Failed to create overlapped file: " << std::hex << hr << std::endl;
        CloseHandle(m_ReceiveOv.hEvent);
        return false;
    }

    return true;
}

ND2_ADAPTER_INFO NDSessionBase::GetAdapterInfo() {
    ND2_ADAPTER_INFO info = { 0 };
    info.InfoVersion = ND_VERSION_2;
    ULONG adapterInfoSize = sizeof(info);
    HRESULT hr = m_pAdapter->Query(&info, &adapterInfoSize);
    if (FAILED(hr)) {
        std::cerr << "Failed to query adapter info: " << std::hex << hr << std::endl;
        return {};
    }

    return info;
}

DWORD NDSessionBase::PrepareSge(ND2_SGE *pSge, const DWORD nSge, char *buff, ULONG buffSize, ULONG headerSize, UINT32 memoryToken) {
    DWORD currSge = 0;
    ULONG buffIdx = 0;
    ULONG currLen = 0;

    while (buffSize != 0 && currSge < nSge) {
        pSge[currSge].Buffer = buff + buffIdx;
        currLen = std::min(buffSize, headerSize);
        pSge[currSge].BufferLength = currLen;
        pSge[currSge].MemoryRegionToken = memoryToken;
        buffSize -= currLen;
        buffIdx += currLen;
        currSge++;
    }

    if (buffSize > 0 && currSge > 0) {
        pSge[currSge - 1].BufferLength += buffSize;
    }
    return currSge;
}

void NDSessionBase::DisconnectConnector() {
    if (m_pConnector) {
        m_pConnector->Disconnect(&m_InitiatorOv);
        m_pConnector->GetOverlappedResult(&m_InitiatorOv, true);
        SafeRelease(m_pConnector);
    }
}

void NDSessionBase::DeregisterMemory() {
    m_pMr->Deregister(&m_InitiatorOv);
    m_pMr->GetOverlappedResult(&m_InitiatorOv, true);
}

HRESULT NDSessionBase::GetResult(IND2CompletionQueue* pCq, OVERLAPPED* pOv) {
    HRESULT hr = pCq->GetOverlappedResult(pOv, true);
    return hr;
}

void NDSessionBase::Shutdown() {
    if (m_pCq) {
        if (m_Ov.hEvent) {
            m_pCq->CancelOverlappedRequests();
        } else {
            // Shouldn't happen
            throw std::runtime_error("CQ is present but OVERLAPPED is not initialized.");
        }
    } else if (m_pReceiveCq && m_pInitiatorCq) {
        if (m_ReceiveOv.hEvent && m_InitiatorOv.hEvent) {
            m_pReceiveCq->CancelOverlappedRequests();
            m_pInitiatorCq->CancelOverlappedRequests();
        } else {
            // Shouldn't happen
            throw std::runtime_error("Completion queues are present but OVERLAPPEDs are not initialized.");
        }
    }


    FlushQP();
    DisconnectConnector();
    DeregisterMemory();
}

HRESULT NDSessionBase::PostReceive(const ND2_SGE* Sge, const DWORD nSge, void *requestContext) {
    HRESULT hr = m_pQp->Receive(requestContext, Sge, nSge);
    return hr;
}

HRESULT NDSessionBase::Write(const ND2_SGE* Sge, const ULONG nSge, UINT64 remoteAddr, UINT32 remoteToken, DWORD flags, void *requestContext) {
    HRESULT hr = m_pQp->Write(requestContext, Sge, nSge, remoteAddr, remoteToken, flags);
    return hr;
}

HRESULT NDSessionBase::Read(const ND2_SGE* Sge, const ULONG nSge, UINT64 remoteAddr, UINT32 remoteToken, DWORD flags, void *requestContext) {
    HRESULT hr = m_pQp->Read(requestContext, Sge, nSge, remoteAddr, remoteToken, flags);
    return hr;
}

HRESULT NDSessionBase::Send(const ND2_SGE* Sge, const ULONG nSge, ULONG flags, void* requestContext) {
    HRESULT hr = m_pQp->Send(requestContext, Sge, nSge, flags);
    return hr;
}

void NDSessionBase::WaitForEventNotification(ULONG notifyFlag) {
    if (m_pCq && m_Ov.hEvent) {
        WaitForEventNotification(m_pCq, &m_Ov, notifyFlag);
        return;
    }
    throw std::runtime_error("Completion queue or overlapped event not initialized.");
}

void NDSessionBase::WaitForEventNotification(IND2CompletionQueue* pCq, OVERLAPPED* pOv, ULONG notifyFlag) {
    HRESULT hr = pCq->Notify(notifyFlag, pOv);
    if (hr == ND_PENDING) {
        hr = pCq->GetOverlappedResult(pOv, true);
    }
}

bool NDSessionBase::WaitForEventNotificationWithTimeout(IND2CompletionQueue* pCq, OVERLAPPED* pOv, ULONG notifyFlag, DWORD timeoutMs) {
    HRESULT hr = pCq->Notify(notifyFlag, pOv);
    if (hr == ND_PENDING) {
        DWORD waitRes = WaitForSingleObject(pOv->hEvent, timeoutMs);
        if (waitRes == WAIT_OBJECT_0) {
            pCq->GetOverlappedResult(pOv, false);
        } else if (waitRes == WAIT_TIMEOUT) {
            pCq->CancelOverlappedRequests();
            pCq->GetOverlappedResult(pOv, true);
            return false;
        }
    }
    return true;
}

ND2_RESULT NDSessionBase::WaitForCompletion(ULONG notifyFlag, bool bBlocking) {
    if (m_pCq == nullptr) {
        throw std::runtime_error("Completion queue not initialized.");
    }
    return WaitForCompletion(m_pCq, &m_Ov, notifyFlag, bBlocking);
}

ND2_RESULT NDSessionBase::WaitForCompletion(IND2CompletionQueue* pCq, OVERLAPPED* pOv, ULONG notifyFlag, bool bBlocking) {
    ND2_RESULT ndRes;

    ULONG numRes = pCq->GetResults(&ndRes, 1);

    if (numRes == 1) {
        return ndRes;
    }
    else if (numRes > 1) abort();

    if (!bBlocking) {
        ndRes.Status = ND_PENDING;
        return ndRes;
    }

    do {
        WaitForEventNotification(pCq, pOv, notifyFlag);
    } while (pCq->GetResults(&ndRes, 1) == 0);

    return ndRes;
}

ND2_RESULT NDSessionBase::WaitForCompletionWithTimeout(IND2CompletionQueue* pCq, OVERLAPPED* pOv, ULONG notifyFlag, DWORD timeoutMs, bool& didTimedOut) {
    ND2_RESULT ndRes;
    ULONG numRes = pCq->GetResults(&ndRes, 1);

    if (numRes == 1) return ndRes;
    else if (numRes > 1) abort();

    while (true) {
        HRESULT hr = pCq->Notify(notifyFlag, pOv);

        if (pCq->GetResults(&ndRes, 1) == 1) {
            if (hr == ND_PENDING) {
                pCq->CancelOverlappedRequests();
                pCq->GetOverlappedResult(pOv, true);
            }
            return ndRes;
        }

        if (hr == ND_PENDING) {
            DWORD waitRes = WaitForSingleObject(pOv->hEvent, timeoutMs);
            if (waitRes == WAIT_OBJECT_0) {
                pCq->GetOverlappedResult(pOv, false);
            } else {
                pCq->CancelOverlappedRequests();
                pCq->GetOverlappedResult(pOv, true);
                didTimedOut = true;
                return ndRes;
            }
        } else if (FAILED(hr)) {
            std::cerr << "Failed to notify completion queue: " << std::hex << hr << std::endl;
            didTimedOut = false;
            return ndRes;
        }
    }
}

bool NDSessionBase::WaitForCompletionAndCheckContext(void *expectedContext, ULONG notifyFlag) {
    IND2CompletionQueue* pCq;
    OVERLAPPED* pOv;

    if (expectedContext == RECV_CTXT) {
        pCq = m_pReceiveCq;
        pOv = &m_ReceiveOv;
    } else {
        pCq = m_pInitiatorCq;
        pOv = &m_InitiatorOv;
    }

    ND2_RESULT ndRes = WaitForCompletion(pCq, pOv, notifyFlag, true);

    if (ndRes.Status == ND_CANCELED) {
        std::cout << "Remote has closed the connection." << std::endl;
        return false;
    } else if (ndRes.Status == ND_IO_TIMEOUT) {
        std::cerr << "Operation timed out." << std::endl;
        std::cerr << "Check for possible outstanding Send request without a posted receive." << std::endl;
        return false;
    }

    if (ND_SUCCESS != ndRes.Status) {
        std::cerr << "Operation failed with status: " << std::hex << ndRes.Status << std::endl;
        #ifdef _DEBUG
        abort();
        #endif
        return false;
    }
    if (expectedContext != ndRes.RequestContext) {
        std::cerr << "Unexpected completion. Check for missing WaitForCompletion() call." << std::endl;
        /*
        #ifdef _DEBUG
        std::terminate();
        #endif
        return false;
        */
        // Mismatch is expected in asynchronous operations.
        return true;
    }

    return true;
}

HRESULT NDSessionBase::WaitForCompletion() {
    ND2_RESULT ndRes = WaitForCompletion(ND_CQ_NOTIFY_ANY, true);
    return ndRes.Status;
}

HRESULT NDSessionBase::FlushQP() {
    HRESULT hr = m_pQp->Flush();
    return hr;
}

HRESULT NDSessionBase::Reject(const void *pPrivateData, DWORD cbPrivateData) {
    HRESULT hr = m_pConnector->Reject(pPrivateData, cbPrivateData);
    return hr;
}

/*
void NDSessionBase::ClearOPs() {
    FlushQP();
    while (true) {
        ND2_RESULT result = WaitForCompletion(ND_CQ_NOTIFY_ANY, false);
        if (result.Status == ND_NO_MORE_ENTRIES || result.Status == ND_PENDING) break;
    }
}
*/

// MARK: NDSessionServerBase
NDSessionServerBase::NDSessionServerBase() : m_pListen(nullptr) {}
NDSessionServerBase::~NDSessionServerBase() {
    SafeRelease(m_pListen);
}

HRESULT NDSessionServerBase::CreateListener() {
    HRESULT hr = m_pAdapter->CreateListener(IID_IND2Listener, m_hAdapterFile, reinterpret_cast<void**>(&m_pListen));
    return hr;
}

HRESULT NDSessionServerBase::Listen(const char* localAddr) {
    struct sockaddr_in addr = { 0 };
    int len = sizeof(addr);
    WSAStringToAddress(const_cast<char*>(localAddr), AF_INET, nullptr, reinterpret_cast<struct sockaddr*>(&addr), &len);

    HRESULT hr = m_pListen->Bind(reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
    if (FAILED(hr)) {
        std::cerr << "Failed to bind listener: " << std::hex << hr << std::endl;
        return hr;
    }
    hr = m_pListen->Listen(1);
    if (FAILED(hr)) {
        std::cerr << "Failed to start listening: " << std::hex << hr << std::endl;
        return hr;
    }

    return hr;
}

HRESULT NDSessionServerBase::GetConnectionRequest() {
    HRESULT hr = m_pListen->GetConnectionRequest(m_pConnector, &m_ReceiveOv);
    if (hr == ND_PENDING) {
        hr = m_pListen->GetOverlappedResult(&m_ReceiveOv, true);
    }
    return hr;
}

HRESULT NDSessionServerBase::Accept(DWORD inboundReadLimit, DWORD outboundReadLimit, const void *pPrivateData, DWORD cbPrivateData) {
    OVERLAPPED* pOv = &m_ReceiveOv;
    HRESULT hr = m_pConnector->Accept(m_pQp, inboundReadLimit, outboundReadLimit, pPrivateData, cbPrivateData, pOv);
    if (hr == ND_PENDING) {
        hr = m_pConnector->GetOverlappedResult(pOv, true);
    }

    return hr;
}

// MARK: NDSessionClientBase

HRESULT NDSessionClientBase::Connect(const char* localAddr, const char* remoteAddr, DWORD inboundReadLimit, DWORD outboundReadLimit, const void *pPrivateData, DWORD cbPrivateData) {
    OVERLAPPED* pOv = &m_ReceiveOv;

    struct sockaddr_in local = { 0 };
    int len = sizeof(local);
    WSAStringToAddress(const_cast<char*>(localAddr), AF_INET, nullptr, reinterpret_cast<struct sockaddr*>(&local), &len);
    local.sin_port = htons(54322);

    struct sockaddr_in remote = { 0 };
    len = sizeof(remote);
    WSAStringToAddress(const_cast<char*>(remoteAddr), AF_INET, nullptr, reinterpret_cast<struct sockaddr*>(&remote), &len);

    HRESULT hr = m_pConnector->Bind(reinterpret_cast<const sockaddr*>(&local), sizeof(local));
    if (hr == ND_PENDING) {
        hr = m_pConnector->GetOverlappedResult(pOv, true);
    }

    if (FAILED(hr)) {
        #ifdef _DEBUG
        abort();
        #endif
    }

    hr = m_pConnector->Connect(m_pQp, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote), inboundReadLimit, outboundReadLimit, pPrivateData, cbPrivateData, pOv);
    if (hr == ND_PENDING) {
        hr = m_pConnector->GetOverlappedResult(pOv, true);
    }

    if (FAILED(hr)) {
        #ifdef _DEBUG
        abort();
        #endif
    }

    return hr;
}

HRESULT NDSessionClientBase::CompleteConnect() {
    OVERLAPPED* pOv = &m_ReceiveOv;

    HRESULT hr = m_pConnector->CompleteConnect(pOv);
    if (hr == ND_PENDING) {
        hr = m_pConnector->GetOverlappedResult(pOv, true);
    }
    return hr;
}