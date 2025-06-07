#include <iostream>
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
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
using sock_t = int;
#define CLOSESOCK(s) close(s)
#define SOCK_ERR   -1
#define SOCK_INV   -1
#endif

int main() {
    const uint16_t PORT = 8080;
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    // 1) Create listening socket
    sock_t listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock == SOCK_INV) {
        std::cerr << "socket() failed\n";
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    // 2) Allow address reuse
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR,
        reinterpret_cast<char*>(&opt), sizeof(opt));

    // 3) Bind
    sockaddr_in srv_addr{};
    srv_addr.sin_family = AF_INET;
    srv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    srv_addr.sin_port = htons(PORT);

    if (bind(listen_sock, (sockaddr*)&srv_addr, sizeof(srv_addr)) == SOCK_ERR) {
        std::cerr << "bind() failed\n";
        CLOSESOCK(listen_sock);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    // 4) Listen
    if (listen(listen_sock, 5) == SOCK_ERR) {
        std::cerr << "listen() failed\n";
        CLOSESOCK(listen_sock);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }
    std::cout << "Server listening on port " << PORT << "\n";

    // 5) Accept client
    sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);
    sock_t client_sock = accept(listen_sock, (sockaddr*)&client_addr, &addr_len);
    if (client_sock == SOCK_INV) {
        std::cerr << "accept() failed\n";
        CLOSESOCK(listen_sock);
#ifdef _WIN32
        WSACleanup();
#endif
        return 1;
    }

    // 6) Print client IP and port
    char client_ip[INET_ADDRSTRLEN];
#ifdef _WIN32
    if (InetNtopA(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN) == nullptr) {
        std::cerr << "InetNtopA failed: " << WSAGetLastError() << "\n";
    }
#else
    if (inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN) == nullptr) {
        perror("inet_ntop");
    }
#endif
    std::cout << "Accepted connection from " << client_ip
        << ":" << ntohs(client_addr.sin_port) << "\n";

    // 7) Receive data
    char buffer[1024];
    int recvd = recv(client_sock, buffer, sizeof(buffer) - 1, 0);
    if (recvd == SOCK_ERR) {
        std::cerr << "recv() failed\n";
    }
    else if (recvd == 0) {
        std::cout << "Client disconnected\n";
    }
    else {
        buffer[recvd] = '\0';
        std::cout << "Received: " << buffer << "\n";
    }

    // 8) Send response
    const char* response = "Hello from the server!";
    int sent = send(client_sock, response, static_cast<int>(strlen(response)), 0);
    if (sent == SOCK_ERR) {
        std::cerr << "send() failed\n";
    }

    // 9) Shutdown and cleanup
#ifdef _WIN32
    shutdown(client_sock, SD_BOTH);
#else
    shutdown(client_sock, SHUT_RDWR);
#endif
    CLOSESOCK(client_sock);
    CLOSESOCK(listen_sock);
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
