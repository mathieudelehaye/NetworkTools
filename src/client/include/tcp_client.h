#pragma once
#include <string>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using sock_t = SOCKET;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
using sock_t = int;
#endif

namespace tcp_client {
    bool initialize_winsock();
    void cleanup_winsock();
    bool connect_to_server(const char* server_ip, uint16_t server_port);
    bool send_message(const std::string& message);
    std::string receive_message();
    void disconnect();
} 