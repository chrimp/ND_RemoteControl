#include <WS2tcpip.h>
#include <cassert>
#include <string>
#include <winsock2.h>
#include <iostream>
#include <thread>

#include "NDSession.hpp"

static unsigned short serverPort = 0;
static bool f_serverReady = false;

bool FindAvailablePort(char* localAddr, unsigned short& port, SOCKET& sock) {
    auto bind_ephemeral = [&](SOCKET& sockOut, unsigned short& portOut) -> bool {
        sockOut = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sockOut == INVALID_SOCKET) {
            std::cerr << "Failed to create socket: " << WSAGetLastError() << std::endl;
            return false;
        }

        sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        inet_pton(AF_INET, localAddr, &addr.sin_addr);
        addr.sin_port = htons(0);

        if (bind(sockOut, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
            std::cerr << "Failed to bind socket: " << WSAGetLastError() << std::endl;
            closesocket(sockOut);
            return false;
        }

        sockaddr_in actual = {};
        int addrLen = sizeof(actual);
        if (getsockname(sockOut, reinterpret_cast<sockaddr*>(&actual), &addrLen) == SOCKET_ERROR) {
            std::cerr << "Failed to getsockname: " << WSAGetLastError() << std::endl;
            closesocket(sockOut);
            return false;
        }

        portOut = ntohs(actual.sin_port);
        return true;
    };

    if (!bind_ephemeral(sock, port)) {
        return false;
    }

    serverPort = port;

    return true;
}

class PortHandoverTestServer : public NDSessionServerBase {
    private:
    bool FindAvailablePort(char* localAddr, unsigned short& port, SOCKET& sock) {
        return ::FindAvailablePort(localAddr, port, sock);
    }

    unsigned long m_BufferSize = 0; 

    public:
    bool Setup(char* localAddr) {
        if (!Initialize(localAddr)) return false;

        ND2_ADAPTER_INFO adapterInfo = GetAdapterInfo();
        if (adapterInfo.AdapterId == 0) return false;
        
        size_t maxSge = adapterInfo.MaxInitiatorSge;

        if (FAILED(CreateCQ(adapterInfo.MaxCompletionQueueDepth))) return false;
        if (FAILED(CreateQP(adapterInfo.MaxReceiveQueueDepth, adapterInfo.MaxInitiatorQueueDepth, adapterInfo.MaxReceiveSge, adapterInfo.MaxInitiatorSge))) return false;
        if (FAILED(CreateMR())) return false;
        

        ULONG flags = ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE;

        m_BufferSize = 1024 * 1024; // 1 MB buffer
        if (FAILED(RegisterDataBuffer(1024 * 1024, flags))) return false;
        if (FAILED(CreateListener())) return false;
        if (FAILED(CreateConnector())) return false;

        return true;
    }

    bool OpenAndAcceptNDConnection(const char* localAddr) {
        if (FAILED(Listen(localAddr))) {
            std::cerr << "Failed to listen on " << localAddr << std::endl;
            return false;
        }

        if (FAILED(GetConnectionRequest())) {
            std::cerr << "Failed to get connection request" << std::endl;
            return false;
        }

        if (FAILED(Accept(1, 1, nullptr, 0))) return false;

        CreateMW();
        Bind(m_Buf, m_BufferSize, ND_OP_FLAG_ALLOW_WRITE | ND_OP_FLAG_ALLOW_READ);

        return true;
    }

    bool OpenAndAcceptTCP(const char* localAddr, SOCKET& listenSock, _Out_ SOCKET& clientSock) {
        std::cout << "TCP: Listening on " << localAddr << std::endl;

        if (listen(listenSock, SOMAXCONN) == SOCKET_ERROR) {
            std::cerr << "Failed to listen on TCP socket: " << WSAGetLastError() << std::endl;
            return false;
        }

        sockaddr_in clientAddr = {};
        int clientAddrLen = sizeof(clientAddr);
        clientSock = accept(listenSock, reinterpret_cast<sockaddr*>(&clientAddr), &clientAddrLen);
        if (clientSock == INVALID_SOCKET) {
            std::cerr << "Failed to accept TCP connection: " << WSAGetLastError() << std::endl;
            return false;
        }

        char clientIp[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &clientAddr.sin_addr, clientIp, sizeof(clientIp));
        std::cout << "TCP: Accepted connection from " << clientIp << ":" << ntohs(clientAddr.sin_port) << std::endl;

        //std::cout << "TCP: Ending connection." << std::endl;
        //closesocket(clientSock);

        return true;
    }

    void RunTest(char* localAddr) {
        unsigned short port = 0;
        SOCKET listenSock = INVALID_SOCKET;
        SOCKET clientSock = INVALID_SOCKET;
        
        if (!FindAvailablePort(localAddr, port, listenSock)) {
            std::cerr << "Failed to find available port" << std::endl;
            return;
        }

        std::cout << "Found available port: " << port << std::endl;
        
        Setup(localAddr);
        std::string addrPort = std::string(localAddr) + ":" + std::to_string(port);

        assert(OpenAndAcceptTCP(addrPort.c_str(), listenSock, clientSock));

        assert(OpenAndAcceptNDConnection(addrPort.c_str()));

        //closesocket(listenSock);

        std::cout << "Test successful." << std::endl;
        std::cout << addrPort << std::endl;
    }
};

class PortHandoverTestClient : public NDSessionClientBase {
    private:
    unsigned long m_BufferSize = 0; 

    public:
    bool Setup(char* localAddr) {
        if (!Initialize(localAddr)) return false;

        ND2_ADAPTER_INFO adapterInfo = GetAdapterInfo();
        if (adapterInfo.AdapterId == 0) return false;
        
        size_t maxSge = adapterInfo.MaxInitiatorSge;

        if (FAILED(CreateCQ(adapterInfo.MaxCompletionQueueDepth))) return false;
        if (FAILED(CreateQP(adapterInfo.MaxReceiveQueueDepth, adapterInfo.MaxInitiatorQueueDepth, adapterInfo.MaxReceiveSge, adapterInfo.MaxInitiatorSge))) return false;
        if (FAILED(CreateMR())) return false;
        

        ULONG flags = ND_MR_FLAG_ALLOW_LOCAL_WRITE | ND_MR_FLAG_ALLOW_REMOTE_WRITE;

        m_BufferSize = 1024 * 1024; // 1 MB buffer
        if (FAILED(RegisterDataBuffer(1024 * 1024, flags))) return false;
        if (FAILED(CreateConnector())) return false;

        return true;
    }

    bool ConnectToServerTCP(const char* ip, _Out_ SOCKET& sock) {
        sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sock == INVALID_SOCKET) {
            std::cerr << "Failed to create TCP socket: " << WSAGetLastError() << std::endl;
            return false;
        }

        sockaddr_in serverAddr = {};
        serverAddr.sin_family = AF_INET;
        inet_pton(AF_INET, ip, &serverAddr.sin_addr);
        serverAddr.sin_port = htons(serverPort);

        int res = connect(sock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));
        if (res == SOCKET_ERROR) {
            std::cerr << "Failed to connect to server: " << WSAGetLastError() << std::endl;
            closesocket(sock);
            return false;
        }

        return true;
    }

    bool ConnectToServerND(const char* localAddr, const char* serverAddr) {
        std::this_thread::sleep_for(std::chrono::seconds(1)); // Give the server a moment to start
        ND2_SGE sge = {};
        sge.Buffer = m_Buf;
        sge.BufferLength = m_BufferSize;
        sge.MemoryRegionToken = m_pMr->GetLocalToken();
        if (FAILED(PostReceive(&sge, 1, RECV_CTXT))) {
            std::cerr << "Failed to post receive" << std::endl;
            return false;
        }

        std::cout << "ND: Connecting to server at " << serverAddr << std::endl;

        if (FAILED(Connect(localAddr, serverAddr, 1, 1, nullptr, 0))) {
            std::cerr << "Failed to connect to server" << std::endl;
            return false;
        }

        if (FAILED(CompleteConnect())) {
            std::cerr << "Failed to complete connection" << std::endl;
            return false;
        }

        std::cout << "Connected to server via ND" << std::endl;
        CreateMW();
        Bind(m_Buf, m_BufferSize, ND_OP_FLAG_ALLOW_WRITE | ND_OP_FLAG_ALLOW_READ);

        return true;
    }

    bool RunTest(char* localAddr, const char* serverAddr) {
        
        SOCKET tcpSock = INVALID_SOCKET;
        if (!ConnectToServerTCP(serverAddr, tcpSock)) {
            std::cerr << "Failed to connect to server via TCP" << std::endl;
            return false;
        }

        if (!Setup(localAddr)) {
            std::cerr << "Failed to setup ND session" << std::endl;
            return false;
        }

        std::string fullServerAddress = std::string(serverAddr) + ":" + std::to_string(serverPort);

        if (!ConnectToServerND(localAddr, fullServerAddress.c_str())) {
            std::cerr << "Failed to connect to server via ND" << std::endl;
            return false;
        }

        return true;
    }
};

int main(int argc, char* argv[]) {
    WSADATA wsaData;
    int wsaResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
    if (wsaResult != 0) {
        std::cerr << "WSAStartup failed: " << wsaResult << std::endl;
        return 1;
    }

    if (FAILED(NdStartup())) {
        std::cerr << "NdStartup failed." << std::endl;
        WSACleanup();
        return 1;
    }

    std::string localAddr =  "192.168.10.20";

    PortHandoverTestServer test;
    PortHandoverTestClient client;

    std::jthread serverThread([&]() {
        test.RunTest(const_cast<char*>(localAddr.c_str()));
    });

    std::this_thread::sleep_for(std::chrono::seconds(1)); // Give the server a moment to start

    client.RunTest(const_cast<char*>(localAddr.c_str()), const_cast<char*>(localAddr.c_str()));
}