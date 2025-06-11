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
    UDP_VIDEO_PREVIEW = 4,
    EXIT = 5
};

Demo show_menu() {
    while (true) {
        std::cout << "\nServer Menu:\n";
        std::cout << "1. TCP Text Message\n";
        std::cout << "2. UDP Text Message\n";
        std::cout << "3. UDP Video Stream\n";
        std::cout << "4. UDP Video Stream with Preview\n";
        std::cout << "5. Exit\n";
        std::cout << "Enter your choice: ";

        int choice;
        std::cin >> choice;

        switch (choice) {
            case 1:
                return Demo::TCP_TEXT;
            case 2:
                return Demo::UDP_TEXT;
            case 3:
                return Demo::UDP_VIDEO;
            case 4:
                return Demo::UDP_VIDEO_PREVIEW;
            case 5:
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

bool run_udp_video_demo(bool preview) {
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

    // Optimize webcam settings
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 640);  // Set resolution
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 480);
    cap.set(cv::CAP_PROP_FPS, 30);          // Request 30 FPS
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G')); // Use MJPG format if available

    std::cout << "Streaming video. Press ESC to stop.\n";

    if (preview) {
        cv::namedWindow("Server Preview", cv::WINDOW_AUTOSIZE | cv::WINDOW_GUI_NORMAL);
    }

    std::vector<uchar> buffer;
    std::vector<int> params = { 
        cv::IMWRITE_JPEG_QUALITY, 60,      // Lower quality for smaller size
        cv::IMWRITE_JPEG_OPTIMIZE, 1       // Enable optimization
    };

    cv::Mat frame, display_frame;
    bool running = true;

    // Set socket buffer size
    int sndbuff = 65536;
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&sndbuff, sizeof(sndbuff));

    // FPS calculation variables
    const int FPS_WINDOW_SIZE = 30;
    std::queue<std::chrono::steady_clock::time_point> frame_times;
    double current_fps = 0.0;

    while (running) {
        auto start = std::chrono::steady_clock::now();

        cap >> frame;
        if (frame.empty()) {
            std::cerr << "Failed to capture frame\n";
            continue;
        }

        // Calculate FPS
        frame_times.push(start);
        while (frame_times.size() > FPS_WINDOW_SIZE) {
            frame_times.pop();
        }

        if (frame_times.size() >= 2) {
            auto time_diff = std::chrono::duration_cast<std::chrono::milliseconds>(
                frame_times.back() - frame_times.front()).count();
            current_fps = (frame_times.size() - 1) * 1000.0 / time_diff;
        }

        // If preview is enabled, create a copy for display
        if (preview) {
            display_frame = frame.clone();
            // Add resolution and FPS overlay
            std::stringstream info;
            info << "Resolution: " << frame.cols << "x" << frame.rows 
                 << " | FPS: " << std::fixed << std::setprecision(1) << current_fps;
            cv::putText(display_frame, info.str(), cv::Point(10, 30),
                cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 0), 2);
            cv::imshow("Server Preview", display_frame);
        }

        // Compress frame to JPEG
        cv::imencode(".jpg", frame, buffer, params);

        if (buffer.size() > 65000) {
            // If frame is too large, reduce quality
            params[1] = std::max(20, params[1] - 5);
            continue;
        }

        // Send JPEG data
        sendto(sock, reinterpret_cast<char*>(buffer.data()), buffer.size(), 0,
            reinterpret_cast<sockaddr*>(&clientAddr), sizeof(clientAddr));

        // Check for ESC key
        char c = preview ? cv::waitKey(1) : 0;
        if (preview && c == 27) {
            running = false;
        } else if (!preview && _kbhit()) {
            if (_getch() == 27) // ESC key
                running = false;
        }

        // Calculate processing time and adjust delay
        auto end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        
        // Aim for ~33ms per frame (30 FPS)
        if (duration < 33) {
            Sleep(33 - duration);
        }
    }

    if (preview) {
        cv::destroyWindow("Server Preview");
    }
    cap.release();
    closesocket(sock);
    WSACleanup();
    return true;
}

int main(int argc, char* argv[]) {
    const uint16_t SERVER_PORT = 8080;

    // Silence INFO-level plugin-loader messages
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
                success = run_udp_video_demo(false);
                break;

            case Demo::UDP_VIDEO_PREVIEW:
                success = run_udp_video_demo(true);
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
