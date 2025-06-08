#include "../include/udp_server.h"
#include <iostream>

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#define CLOSESOCK(s) closesocket(s)
#define SOCK_ERR   SOCKET_ERROR
#define SOCK_INV   INVALID_SOCKET
#else
#include <unistd.h>
#define CLOSESOCK(s) close(s)
#define SOCK_ERR   -1
#define SOCK_INV   -1
#endif

namespace udp_server {
    static sock_t server_socket = SOCK_INV;

    bool initialize_winsock() {
#ifdef _WIN32
        WSADATA wsaData;
        if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
            std::cerr << "WSAStartup failed\n";
            return false;
        }
#endif
        return true;
    }

    void cleanup_winsock() {
#ifdef _WIN32
        WSACleanup();
#endif
    }

    bool start_server(uint16_t port) {
        // Create UDP socket
        server_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (server_socket == SOCK_INV) {
            std::cerr << "socket() failed\n";
            return false;
        }

        // Bind
        sockaddr_in server_addr{};
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        server_addr.sin_port = htons(port);

        if (bind(server_socket, (sockaddr*)&server_addr, sizeof(server_addr)) == SOCK_ERR) {
            std::cerr << "bind() failed\n";
            CLOSESOCK(server_socket);
            return false;
        }

        std::cout << "UDP Server listening on port " << port << "\n";
        return true;
    }

    bool send_message(const std::string& message, const sockaddr_in& client_addr) {
        int sent = sendto(server_socket, message.c_str(), static_cast<int>(message.length()), 0,
            (sockaddr*)&client_addr, sizeof(client_addr));
        if (sent == SOCK_ERR) {
            std::cerr << "sendto() failed\n";
            return false;
        }
        
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "Sent message to " << client_ip << ":" << ntohs(client_addr.sin_port) << "\n";
        std::cout << "Message: " << message << "\n";
        return true;
    }

    std::pair<std::string, sockaddr_in> receive_message() {
        char buffer[1024];
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        
        int recvd = recvfrom(server_socket, buffer, sizeof(buffer) - 1, 0,
            (sockaddr*)&client_addr, &addr_len);
        
        if (recvd == SOCK_ERR) {
            std::cerr << "recvfrom() failed\n";
            return {"", sockaddr_in{}};
        }
        
        buffer[recvd] = '\0';
        
        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "Received from " << client_ip << ":" << ntohs(client_addr.sin_port) << "\n";
        std::cout << "Message: " << buffer << "\n";
        
        return {std::string(buffer), client_addr};
    }

    void stop_server() {
        if (server_socket != SOCK_INV) {
            CLOSESOCK(server_socket);
            server_socket = SOCK_INV;
        }
    }
} 