#include "tcp_server.h"
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

namespace tcp_server {
    static sock_t server_socket = SOCK_INV;
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

    bool start_server(uint16_t port) {
        // Create socket
        server_socket = socket(AF_INET, SOCK_STREAM, 0);
        if (server_socket == SOCK_INV) {
            std::cerr << "socket() failed\n";
            return false;
        }

        // Allow address reuse
        int opt = 1;
        setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR,
            reinterpret_cast<char*>(&opt), sizeof(opt));

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

        // Listen
        if (listen(server_socket, 5) == SOCK_ERR) {
            std::cerr << "listen() failed\n";
            CLOSESOCK(server_socket);
            return false;
        }

        std::cout << "TCP Server listening on port " << port << "\n";
        return true;
    }

    bool accept_client() {
        sockaddr_in client_addr{};
        socklen_t addr_len = sizeof(client_addr);
        
        client_socket = accept(server_socket, (sockaddr*)&client_addr, &addr_len);
        if (client_socket == SOCK_INV) {
            std::cerr << "accept() failed\n";
            return false;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);
        std::cout << "Accepted TCP connection from " << client_ip 
                  << ":" << ntohs(client_addr.sin_port) << "\n";
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
            std::cout << "Client disconnected\n";
            return "";
        }
        buffer[recvd] = '\0';
        return std::string(buffer);
    }

    void stop_server() {
        if (client_socket != SOCK_INV) {
#ifdef _WIN32
            shutdown(client_socket, SD_BOTH);
#else
            shutdown(client_socket, SHUT_RDWR);
#endif
            CLOSESOCK(client_socket);
            client_socket = SOCK_INV;
        }
        if (server_socket != SOCK_INV) {
            CLOSESOCK(server_socket);
            server_socket = SOCK_INV;
        }
    }
} 