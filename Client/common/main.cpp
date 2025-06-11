#include <iostream>
#include <string>
#include <limits>
#include <queue>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <opencv2/opencv.hpp>
// Fix for Windows max macro conflict
#define NOMINMAX
#ifdef _WIN32
#include <conio.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#endif
#include "../include/tcp_client.h"
#include "../include/udp_client.h"

#define VIDEO_PORT 12345
#define BUFFER_SIZE 65536

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

bool run_tcp_text_demo(const char* server_ip, uint16_t server_port) {
    // Initialize TCP client
    if (!tcp_client::initialize_winsock()) {
        return false;
    }

    // Connect to server
    if (!tcp_client::connect_to_server(server_ip, server_port)) {
        tcp_client::cleanup_winsock();
        return false;
    }

    // Send message
    if (!tcp_client::send_message("Hello from TCP client!")) {
        tcp_client::disconnect();
        tcp_client::cleanup_winsock();
        return false;
    }

    // Receive response
    std::string response = tcp_client::receive_message();
    if (!response.empty()) {
        std::cout << "Server response: " << response << "\n";
    }

    // Cleanup
    tcp_client::disconnect();
    tcp_client::cleanup_winsock();
    return true;
}

bool run_udp_text_demo(const char* server_ip, uint16_t server_port) {
    // Initialize UDP client
    if (!udp_client::initialize_winsock()) {
        return false;
    }

    // Create socket and set server address
    if (!udp_client::create_socket(server_ip, server_port)) {
        udp_client::cleanup_winsock();
        return false;
    }

    // Send message
    if (!udp_client::send_message("Hello from UDP client!")) {
        udp_client::close_socket();
        udp_client::cleanup_winsock();
        return false;
    }

    // Receive response
    std::string response = udp_client::receive_message();
    if (!response.empty()) {
        std::cout << "Server response: " << response << "\n";
    }

    // Cleanup
    udp_client::close_socket();
    udp_client::cleanup_winsock();
    return true;
}

bool run_udp_video_demo(const char* server_ip) {
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

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(VIDEO_PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Failed to bind socket\n";
        closesocket(sock);
        WSACleanup();
        return false;
    }

    std::cout << "Receiving video stream. Press ESC to stop.\n";

    char buffer[BUFFER_SIZE];
    sockaddr_in senderAddr;
    int senderLen = sizeof(senderAddr);

    // FPS calculation variables
    const int FPS_WINDOW_SIZE = 30; // Calculate FPS over 30 frames
    std::queue<std::chrono::steady_clock::time_point> frame_times;
    int total_frames = 0;
    double current_fps = 0.0;
    auto last_fps_print = std::chrono::steady_clock::now();
    const auto FPS_PRINT_INTERVAL = std::chrono::seconds(1); // Update FPS display every second

    while (true) {
        int bytesReceived = recvfrom(sock, buffer, BUFFER_SIZE, 0,
            reinterpret_cast<sockaddr*>(&senderAddr), &senderLen);

        if (bytesReceived > 0) {
            // Record frame arrival time for FPS calculation
            auto current_time = std::chrono::steady_clock::now();
            frame_times.push(current_time);
            total_frames++;

            // Maintain our sliding window of frame times
            while (frame_times.size() > FPS_WINDOW_SIZE) {
                frame_times.pop();
            }

            // Calculate FPS if we have enough frames
            if (frame_times.size() >= 2) {
                auto time_diff = std::chrono::duration_cast<std::chrono::milliseconds>(
                    frame_times.back() - frame_times.front()).count();
                current_fps = (frame_times.size() - 1) * 1000.0 / time_diff;
            }

            // Decode and display the frame
            std::vector<uchar> data(buffer, buffer + bytesReceived);
            cv::Mat img = cv::imdecode(data, cv::IMREAD_COLOR);

            if (!img.empty()) {
                // Create info text with resolution and FPS
                std::stringstream info;
                info << "Resolution: " << img.cols << "x" << img.rows 
                     << " | FPS: " << std::fixed << std::setprecision(1) << current_fps;

                // Draw the info text on the frame
                cv::putText(img, info.str(), cv::Point(10, 30),
                    cv::FONT_HERSHEY_SIMPLEX, 1.0, cv::Scalar(0, 255, 0), 2);

                // Display the frame
                cv::imshow("Received Video Stream", img);
            }
        }

        if (cv::waitKey(1) == 27) break; // ESC to quit
    }

    cv::destroyAllWindows();
    closesocket(sock);
    WSACleanup();
    return true;
}

int main(int argc, char* argv[]) {
    // Allow optional server IP as first argument (default = localhost)
    const char* SERVER_IP = (argc > 1 ? argv[1] : "127.0.0.1");
    const uint16_t SERVER_PORT = 8080;

    while (true) {
        Demo choice = show_menu();
        bool success = false;

        switch (choice) {
            case Demo::TCP_TEXT:
                success = run_tcp_text_demo(SERVER_IP, SERVER_PORT);
                break;

            case Demo::UDP_TEXT:
                success = run_udp_text_demo(SERVER_IP, SERVER_PORT);
                break;

            case Demo::UDP_VIDEO:
                success = run_udp_video_demo(SERVER_IP);
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
