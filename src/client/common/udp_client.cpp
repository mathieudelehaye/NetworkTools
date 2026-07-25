#include "udp_client.h"
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

namespace udp_client {
    static sock_t client_socket = SOCK_INV;
    static sockaddr_in server_addr{};

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

    bool create_socket(const char* server_ip, uint16_t server_port) {
        // Create UDP socket
        client_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (client_socket == SOCK_INV) {
            std::cerr << "socket() failed\n";
            return false;
        }

        // Prepare server address
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(server_port);
        if (inet_pton(AF_INET, server_ip, &server_addr.sin_addr) != 1) {
            std::cerr << "Invalid address: " << server_ip << "\n";
            CLOSESOCK(client_socket);
            return false;
        }

        std::cout << "UDP socket created for server " << server_ip << ":" << server_port << "\n";
        return true;
    }

    bool send_message(const std::string& message) {
        int sent = sendto(client_socket, message.c_str(), static_cast<int>(message.length()), 0,
            (sockaddr*)&server_addr, sizeof(server_addr));
        if (sent == SOCK_ERR) {
            std::cerr << "sendto() failed\n";
            return false;
        }
        std::cout << "Sent message: " << message << "\n";
        return true;
    }

    std::string receive_message() {
        char buffer[1024];
        sockaddr_in from_addr{};
        socklen_t from_len = sizeof(from_addr);
        
        int recvd = recvfrom(client_socket, buffer, sizeof(buffer) - 1, 0,
            (sockaddr*)&from_addr, &from_len);
        
        if (recvd == SOCK_ERR) {
            std::cerr << "recvfrom() failed\n";
            return "";
        }
        
        buffer[recvd] = '\0';
        
        char sender_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &from_addr.sin_addr, sender_ip, INET_ADDRSTRLEN);
        std::cout << "Received from " << sender_ip << ":" << ntohs(from_addr.sin_port) << "\n";
        
        return std::string(buffer);
    }

    void close_socket() {
        if (client_socket != SOCK_INV) {
            CLOSESOCK(client_socket);
            client_socket = SOCK_INV;
        }
    }
} 