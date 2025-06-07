#include <iostream>
#include <conio.h>
#include <cstring>
#ifdef _WIN32
#include <conio.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using sock_t = SOCKET;
#define CLOSESOCK(s) closesocket(s)
#define SOCK_ERR   SOCKET_ERROR
#define SOCK_INV   INVALID_SOCKET
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <unistd.h>
using sock_t = int;
#define CLOSESOCK(s) close(s)
#define SOCK_ERR   -1
#define SOCK_INV   -1
#endif

int main(int argc, char* argv[]) {
    // Allow optional server IP as first argument (default = localhost)
    const char* SERVER_IP = (argc > 1 ? argv[1] : "127.0.0.1");
    const uint16_t SERVER_PORT = 8080;

#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    // 1) Create socket
    sock_t sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd == SOCK_INV) {
        std::cerr << "socket() failed\n";
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    // 2) Prepare server address struct
    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(SERVER_PORT);
    if (inet_pton(AF_INET, SERVER_IP, &serverAddr.sin_addr) != 1) {
        std::cerr << "Invalid address: " << SERVER_IP << "\n";
        CLOSESOCK(sockfd);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    // 3) Connect
    if (connect(sockfd, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCK_ERR) {
        std::cerr << "connect() failed\n";
        CLOSESOCK(sockfd);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    // 4) Print connection info
    char ipStr[INET_ADDRSTRLEN];
#ifdef _WIN32
    // Use ANSI version on Windows
    if (InetNtopA(AF_INET, &serverAddr.sin_addr, ipStr, INET_ADDRSTRLEN) == nullptr) {
        std::cerr << "InetNtopA failed: " << WSAGetLastError() << "\n";
    }
#else
    if (inet_ntop(AF_INET, &serverAddr.sin_addr, ipStr, INET_ADDRSTRLEN) == nullptr) {
        perror("inet_ntop");
    }
#endif
    std::cout << "Connected to " << ipStr << ":" << SERVER_PORT << "\n";

    // 5) Send data
    const char* message = "Hello, server!";
    int sent = send(sockfd, message, static_cast<int>(strlen(message)), 0);
    if (sent == SOCK_ERR) {
        std::cerr << "send() failed\n";
        CLOSESOCK(sockfd);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    std::cout << "Sent message: " << message << "\n";

    // 6) Receive response
    char buffer[1024];
    int recvd = recv(sockfd, buffer, sizeof(buffer) - 1, 0);
    if (recvd == SOCK_ERR) {
        std::cerr << "recv() failed\n";
    }
    else if (recvd == 0) {
        std::cout << "Server closed connection\n";
    }
    else {
        buffer[recvd] = '\0';
        std::cout << "Received: " << buffer << "\n";
    }

    // 7) Shutdown and cleanup
#ifdef _WIN32
    shutdown(sockfd, SD_BOTH);
#else
    shutdown(sockfd, SHUT_RDWR);
#endif
    CLOSESOCK(sockfd);
#ifdef _WIN32
    WSACleanup();
#endif

    std::cout << "Press any key to exit";
#ifdef _WIN32
    int ch = _getch();
#else
    std::cin.get();
#endif          

    return 0;
}
