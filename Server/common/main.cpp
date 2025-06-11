#include <iostream>
#include <string>
#include <limits>
#include <opencv2/opencv.hpp>
// Fix for Windows max macro conflict
#define NOMINMAX
#ifdef _WIN32
#include <conio.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>  // For Sleep()
#pragma comment(lib, "ws2_32.lib")
#endif
#include "../include/tcp_server.h"
#include "../include/udp_server.h"

#define VIDEO_PORT 12345
#define CLIENT_IP "127.0.0.1"

enum class Demo {
    TCP_TEXT = 1,
    UDP_TEXT = 2,
    UDP_VIDEO = 3,
    EXIT = 4
};

Demo show_menu() {
    while (true) {
        std::cout << "\nSelect Demo:\n";
        std::cout << "1. TCP Text Message\n";
        std::cout << "2. UDP Text Message\n";
        std::cout << "3. UDP Video Stream\n";
        std::cout << "4. Exit\n";
        std::cout << "Choice (1-4): ";
        
        char choice;
        std::cin >> choice;
        std::cin.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
        
        switch (choice) {
            case '1':
                return Demo::TCP_TEXT;
            case '2':
                return Demo::UDP_TEXT;
            case '3':
                return Demo::UDP_VIDEO;
            case '4':
                return Demo::EXIT;
            default:
                std::cout << "Invalid choice. Please try again.\n";
        }
    }
}

bool run_tcp_text_demo(uint16_t port) {
    // Initialize TCP server
    if (!tcp_server::initialize_winsock()) {
        return false;
    }

    // Start server
    if (!tcp_server::start_server(port)) {
        tcp_server::cleanup_winsock();
        return false;
    }

    std::cout << "TCP Server started. Waiting for client...\n";

    // Accept client
    if (!tcp_server::accept_client()) {
        tcp_server::stop_server();
        tcp_server::cleanup_winsock();
        return false;
    }

    // Receive message
    std::string message = tcp_server::receive_message();
    if (!message.empty()) {
        std::cout << "Received message: " << message << "\n";
        
        // Send response
        if (!tcp_server::send_message("Hello from TCP server!")) {
            tcp_server::stop_server();
            tcp_server::cleanup_winsock();
            return false;
        }
    }

    // Cleanup
    tcp_server::stop_server();
    tcp_server::cleanup_winsock();
    return true;
}

bool run_udp_text_demo(uint16_t port) {
    // Initialize UDP server
    if (!udp_server::initialize_winsock()) {
        return false;
    }

    // Start server
    if (!udp_server::start_server(port)) {
        udp_server::cleanup_winsock();
        return false;
    }

    std::cout << "UDP Server started. Waiting for client...\n";

    // Receive message
    auto [message, client_addr] = udp_server::receive_message();
    if (!message.empty()) {
        std::cout << "Received message: " << message << "\n";
        
        // Send response
        if (!udp_server::send_message("Hello from UDP server!", client_addr)) {
            udp_server::stop_server();
            udp_server::cleanup_winsock();
            return false;
        }
    }

    // Cleanup
    udp_server::stop_server();
    udp_server::cleanup_winsock();
    return true;
}

bool run_udp_video_demo() {
    // Init Winsock
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "Failed to initialize Winsock\n";
        return false;
    }

    // Create socket
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        std::cerr << "Failed to create socket\n";
        WSACleanup();
        return false;
    }

    sockaddr_in clientAddr;
    clientAddr.sin_family = AF_INET;
    clientAddr.sin_port = htons(VIDEO_PORT);
    if (inet_pton(AF_INET, CLIENT_IP, &clientAddr.sin_addr) != 1) {
        std::cerr << "Failed to set client IP\n";
        closesocket(sock);
        WSACleanup();
        return false;
    }

    // Open webcam
    cv::VideoCapture cap(0);
    if (!cap.isOpened()) {
        std::cerr << "Could not open webcam.\n";
        closesocket(sock);
        WSACleanup();
        return false;
    }

    std::cout << "Streaming video. Press ESC to stop.\n";

    std::vector<uchar> buffer;
    std::vector<int> params = { cv::IMWRITE_JPEG_QUALITY, 70 };

    cv::Mat frame;
    bool running = true;
    while (running) {
        cap >> frame;
        if (frame.empty()) {
            std::cerr << "Failed to capture frame\n";
            continue;
        }

        // Compress frame to JPEG
        cv::imencode(".jpg", frame, buffer, params);

        if (buffer.size() > 65000) {
            std::cerr << "Frame too large for UDP. Skipping.\n";
            continue;
        }

        // Send JPEG data
        sendto(sock, reinterpret_cast<char*>(buffer.data()), static_cast<int>(buffer.size()), 0,
            reinterpret_cast<sockaddr*>(&clientAddr), sizeof(clientAddr));

        // Check for ESC key without displaying the frame
        if (_kbhit()) {
            if (_getch() == 27) // ESC key
                running = false;
        }

        // Small delay to control frame rate
        Sleep(30); // ~33 FPS
    }

    cap.release();
    closesocket(sock);
    WSACleanup();
    return true;
}

int main() {
    const uint16_t SERVER_PORT = 8080;

    // Silence INFO?level plugin?loader messages
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_WARNING);

    while (true) {
        Demo choice = show_menu();
        bool success = false;

        switch (choice) {
            case Demo::TCP_TEXT:
                success = run_tcp_text_demo(SERVER_PORT);
                break;

            case Demo::UDP_TEXT:
                success = run_udp_text_demo(SERVER_PORT);
                break;

            case Demo::UDP_VIDEO:
                success = run_udp_video_demo();
                break;

            case Demo::EXIT:
                std::cout << "Exiting...\n";
                return 0;
        }

        if (success) {
            std::cout << "Demo completed successfully!\n";
        }
        else {
            std::cout << "Demo failed!\n";
        }

        std::cout << "\nPress any key to continue...";
#ifdef _WIN32
        _getch();
#else
        std::cin.get();
#endif
    }

    return 0;
}
