#include <iostream>
#include <string>
#include <limits>
// Fix for Windows max macro conflict
#define NOMINMAX
#ifdef _WIN32
#include <conio.h>
#endif
#include "../include/tcp_client.h"
#include "../include/udp_client.h"

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

int main(int argc, char* argv[]) {
    // Allow optional server IP as first argument (default = localhost)
    const char* SERVER_IP = (argc > 1 ? argv[1] : "127.0.0.1");
    const uint16_t SERVER_PORT = 8080;

    // Show protocol selection menu
    Protocol protocol = show_menu();
    
    bool success = false;
    if (protocol == Protocol::TCP) {
        // Initialize TCP client
        if (!tcp_client::initialize_winsock()) {
            return 1;
        }

        // Connect to server
        if (!tcp_client::connect_to_server(SERVER_IP, SERVER_PORT)) {
            tcp_client::cleanup_winsock();
            return 1;
        }

        // Send message
        if (!tcp_client::send_message("Hello from TCP client!")) {
            tcp_client::disconnect();
            tcp_client::cleanup_winsock();
            return 1;
        }

        // Receive response
        std::string response = tcp_client::receive_message();
        if (!response.empty()) {
            std::cout << "Server response: " << response << "\n";
            success = true;
        }

        // Cleanup
        tcp_client::disconnect();
        tcp_client::cleanup_winsock();
    }
    else { // UDP
        // Initialize UDP client
        if (!udp_client::initialize_winsock()) {
            return 1;
        }

        // Create socket and set server address
        if (!udp_client::create_socket(SERVER_IP, SERVER_PORT)) {
            udp_client::cleanup_winsock();
            return 1;
        }

        // Send message
        if (!udp_client::send_message("Hello from UDP client!")) {
            udp_client::close_socket();
            udp_client::cleanup_winsock();
            return 1;
        }

        // Receive response
        std::string response = udp_client::receive_message();
        if (!response.empty()) {
            std::cout << "Server response: " << response << "\n";
            success = true;
        }

        // Cleanup
        udp_client::close_socket();
        udp_client::cleanup_winsock();
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
