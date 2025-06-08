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

namespace tcp_server {
    bool initialize_winsock();
    void cleanup_winsock();
    bool start_server(uint16_t port);
    bool accept_client();
    bool send_message(const std::string& message);
    std::string receive_message();
    void stop_server();
} 