#include <iostream>
#include <string>
#include <limits>
// Fix for Windows max macro conflict
#define NOMINMAX
#ifdef _WIN32
#include <conio.h>
#endif
#include "tcp_server.h"
#include "udp_server.h"

enum class Protocol {
    TCP,
    UDP
};

Protocol show_menu() {
    while (true) {
        std::cout << "\nSelect Protocol:\n";
        std::cout << "1. TCP\n";
        std::cout << "2. UDP\n";
        std::cout << "Choice (1-2): ";
        
        char choice;
        std::cin >> choice;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        
        switch (choice) {
            case '1':
                return Protocol::TCP;
            case '2':
                return Protocol::UDP;
            default:
                std::cout << "Invalid choice. Please try again.\n";
        }
    }
}

int main() {
    const uint16_t PORT = 8080;

    // Show protocol selection menu
    Protocol protocol = show_menu();
    
    bool success = false;
    if (protocol == Protocol::TCP) {
        // Initialize TCP server
        if (!tcp_server::initialize_winsock()) {
            return 1;
        }

        // Start server
        if (!tcp_server::start_server(PORT)) {
            tcp_server::cleanup_winsock();
            return 1;
        }

        // Accept client
        if (!tcp_server::accept_client()) {
            tcp_server::stop_server();
            tcp_server::cleanup_winsock();
            return 1;
        }

        // Receive message
        std::string message = tcp_server::receive_message();
        if (!message.empty()) {
            std::cout << "Received message: " << message << "\n";
            
            // Send response
            if (tcp_server::send_message("Hello from TCP server!")) {
                success = true;
            }
        }

        // Cleanup
        tcp_server::stop_server();
        tcp_server::cleanup_winsock();
    }
    else { // UDP
        // Initialize UDP server
        if (!udp_server::initialize_winsock()) {
            return 1;
        }

        // Start server
        if (!udp_server::start_server(PORT)) {
            udp_server::cleanup_winsock();
            return 1;
        }

        // Receive message
        auto [message, client_addr] = udp_server::receive_message();
        if (!message.empty()) {
            std::cout << "Received message: " << message << "\n";
            
            // Send response
            if (udp_server::send_message("Hello from UDP server!", client_addr)) {
                success = true;
            }
        }

        // Cleanup
        udp_server::stop_server();
        udp_server::cleanup_winsock();
    }

    if (success) {
        std::cout << "Communication completed successfully!\n";
    }

    std::cout << "Press any key to exit";
#ifdef _WIN32
    int ch = _getch();
#else
    std::cin.get();
#endif

    return success ? 0 : 1;
}
