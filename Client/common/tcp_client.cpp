#include "tcp_client.h"
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

namespace tcp_client {
    static sock_t client_socket = SOCK_INV;

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

    bool connect_to_server(const char* server_ip, uint16_t server_port) {
        // Create socket
        client_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (client_socket == SOCK_INV) {
            std::cerr << "socket() failed\n";
            return false;
        }

        // Prepare server address
        sockaddr_in serverAddr{};
        serverAddr.sin_family = AF_INET;
        serverAddr.sin_port = htons(server_port);
        if (inet_pton(AF_INET, server_ip, &serverAddr.sin_addr) != 1) {
            std::cerr << "Invalid address: " << server_ip << "\n";
            CLOSESOCK(client_socket);
            return false;
        }

        // Connect
        if (connect(client_socket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCK_ERR) {
            std::cerr << "connect() failed\n";
            CLOSESOCK(client_socket);
            return false;
        }

        std::cout << "Connected to " << server_ip << ":" << server_port << "\n";
        return true;
    }

    bool send_message(const std::string& message) {
        int sent = send(client_socket, message.c_str(), static_cast<int>(message.length()), 0);
        if (sent == SOCK_ERR) {
            std::cerr << "send() failed\n";
            return false;
        }
        std::cout << "Sent message: " << message << "\n";
        return true;
    }

    std::string receive_message() {
        char buffer[1024];
        int recvd = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (recvd == SOCK_ERR) {
            std::cerr << "recv() failed\n";
            return "";
        }
        else if (recvd == 0) {
            std::cout << "Server closed connection\n";
            return "";
        }
        buffer[recvd] = '\0';
        return std::string(buffer);
    }

    void disconnect() {
        if (client_socket != SOCK_INV) {
#ifdef _WIN32
            shutdown(client_socket, SD_BOTH);
#else
            shutdown(client_socket, SHUT_RDWR);
#endif
            CLOSESOCK(client_socket);
            client_socket = SOCK_INV;
        }
    }
} 