#include <iostream>
#include <string>
#include <limits>
#include <opencv2/opencv.hpp>
#include <queue>
#include <chrono>
#include <iomanip>
#include <sstream>
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

    // Print client address info
    char client_ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &clientAddr.sin_addr, client_ip_str, INET_ADDRSTRLEN);
    std::cout << "Sending video to client at " << client_ip_str << ":" << ntohs(clientAddr.sin_port) << std::endl;

    // Enable broadcast (in case client is on broadcast address)
    int broadcast = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, (char*)&broadcast, sizeof(broadcast)) < 0) {
        std::cerr << "Failed to set broadcast option\n";
    }

    // Increase send buffer size
    int sendbuf = 262144; // 256KB buffer
    if (setsockopt(sock, SOL_SOCKET, SO_SNDBUF, (char*)&sendbuf, sizeof(sendbuf)) < 0) {
        std::cerr << "Failed to set send buffer size\n";
    }

    // Set non-blocking mode
    u_long mode = 1;
    if (ioctlsocket(sock, FIONBIO, &mode) == SOCKET_ERROR) {
        std::cerr << "Failed to set non-blocking mode\n";
    }

    // Open webcam with DirectShow backend
    cv::VideoCapture cap(0, cv::CAP_DSHOW);
    if (!cap.isOpened()) {
        std::cerr << "Could not open webcam.\n";
        closesocket(sock);
        WSACleanup();
        return false;
    }

    // Configure camera for high performance
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 1920);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 1080);
    cap.set(cv::CAP_PROP_FPS, 30);

    // Read back actual camera settings
    double actualWidth = cap.get(cv::CAP_PROP_FRAME_WIDTH);
    double actualHeight = cap.get(cv::CAP_PROP_FRAME_HEIGHT);
    double actualFPS = cap.get(cv::CAP_PROP_FPS);

    std::cout << "Camera initialized with settings:\n"
              << "Resolution: " << actualWidth << "x" << actualHeight << "\n"
              << "FPS: " << actualFPS << "\n"
              << "Streaming video. Press ESC to stop.\n";

    if (preview) {
        cv::namedWindow("Server Preview", cv::WINDOW_AUTOSIZE | cv::WINDOW_GUI_NORMAL);
    }

    std::vector<uchar> buffer;
    std::vector<int> params;
    params.push_back(cv::IMWRITE_JPEG_QUALITY);
    params.push_back(85);
    params.push_back(cv::IMWRITE_JPEG_OPTIMIZE);
    params.push_back(1);

    cv::Mat frame, display_frame;
    bool running = true;

    // FPS and rate control variables
    const int TARGET_FPS = 30;
    const int FPS_WINDOW_SIZE = 30;
    const double FRAME_TIME = 1000.0 / TARGET_FPS;
    const size_t MAX_CHUNK_SIZE = 58000; // Reduced to avoid fragmentation
    const size_t MAX_PENDING_FRAMES = 2;
    std::queue<std::chrono::steady_clock::time_point> frame_times;
    double current_fps = 0.0;

    // Network congestion control
    size_t consecutive_errors = 0;
    const size_t ERROR_THRESHOLD = 5;
    int current_quality = 85;
    
    // Debug variables for network statistics
    size_t total_bytes_sent = 0;
    size_t total_chunks_sent = 0;
    size_t dropped_frames = 0;
    auto last_stats = std::chrono::steady_clock::now();

    while (running) {
        auto frame_start = std::chrono::steady_clock::now();

        cap >> frame;
        if (frame.empty()) {
            std::cerr << "Failed to capture frame\n";
            continue;
        }

        // Calculate FPS
        frame_times.push(frame_start);
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
                 << " | FPS: " << std::fixed << std::setprecision(1) << current_fps
                 << " | Target: " << actualFPS
                 << " | Quality: " << current_quality;
            cv::putText(display_frame, info.str(), cv::Point(10, 30),
                cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 0), 2);
            cv::imshow("Server Preview", display_frame);
        }

        // Compress frame to JPEG with dynamic quality
        params[1] = current_quality;
        cv::imencode(".jpg", frame, buffer, params);

        // Split frame into chunks with headers
        const size_t HEADER_SIZE = 12; // 4 bytes frame ID, 4 bytes chunk ID, 4 bytes total_chunks
        size_t total_size = buffer.size();
        size_t num_chunks = (total_size + MAX_CHUNK_SIZE - 1) / MAX_CHUNK_SIZE;
        static uint32_t frame_id = 0;

        // Debug output
        static auto last_debug = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_debug).count() >= 1) {
            std::cout << "Server: Frame " << frame_id << " size: " << total_size 
                      << " bytes, chunks: " << num_chunks 
                      << ", quality: " << current_quality << std::endl;
            last_debug = now;
        }

        // Print network statistics every second
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_stats).count() >= 1) {
            std::cout << "Network stats - Sent: " << total_bytes_sent / 1024 << " KB, "
                     << "Chunks: " << total_chunks_sent << ", "
                     << "Average chunk size: " << (total_chunks_sent ? total_bytes_sent / total_chunks_sent : 0)
                     << " bytes, Dropped frames: " << dropped_frames << std::endl;
            total_bytes_sent = 0;
            total_chunks_sent = 0;
            dropped_frames = 0;
            last_stats = now;
        }

        // Prepare header buffer
        std::vector<char> chunk_buffer(MAX_CHUNK_SIZE + HEADER_SIZE);
        
        // Send all chunks for this frame
        size_t offset = 0;
        bool frame_sent = true;
        for (size_t chunk_id = 0; chunk_id < num_chunks; chunk_id++) {
            size_t chunk_size = std::min(MAX_CHUNK_SIZE, total_size - offset);
            
            // Write header (frame_id, chunk_id, total_chunks)
            uint32_t frame_id_net = htonl(frame_id);
            uint32_t chunk_id_net = htonl(static_cast<uint32_t>(chunk_id));
            uint32_t total_chunks_net = htonl(static_cast<uint32_t>(num_chunks));
            memcpy(chunk_buffer.data(), &frame_id_net, 4);
            memcpy(chunk_buffer.data() + 4, &chunk_id_net, 4);
            memcpy(chunk_buffer.data() + 8, &total_chunks_net, 4);
            
            // Copy chunk data
            memcpy(chunk_buffer.data() + HEADER_SIZE, buffer.data() + offset, chunk_size);
            
            // Send chunk with timeout using select
            fd_set writefds;
            FD_ZERO(&writefds);
            FD_SET(sock, &writefds);
            
            timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 5000; // 5ms timeout
            
            if (select(0, nullptr, &writefds, nullptr, &tv) > 0) {
                int sent = sendto(sock, chunk_buffer.data(), static_cast<int>(chunk_size + HEADER_SIZE), 0,
                    reinterpret_cast<sockaddr*>(&clientAddr), sizeof(clientAddr));
                    
                if (sent == SOCKET_ERROR) {
                    int error = WSAGetLastError();
                    if (error != WSAEWOULDBLOCK) {
                        consecutive_errors++;
                        frame_sent = false;
                        if (consecutive_errors >= ERROR_THRESHOLD && current_quality > 60) {
                            current_quality = std::max(60, static_cast<int>(current_quality - 5));
                            consecutive_errors = 0;
                        }
                    }
                } else {
                    consecutive_errors = 0;
                    total_bytes_sent += sent;
                    total_chunks_sent++;
                }
            } else {
                frame_sent = false;
                break;
            }
            
            offset += chunk_size;
        }
        
        if (!frame_sent) {
            dropped_frames++;
        } else if (consecutive_errors == 0 && current_quality < 85) {
            // Gradually increase quality if network is stable
            current_quality = std::min(85, static_cast<int>(current_quality + 1));
        }
        
        frame_id++;

        // Check for ESC key and handle input
        if (preview) {
            char c = static_cast<char>(cv::waitKey(1));
            if (c == 27) running = false;  // ESC key
        } else if (_kbhit()) {
            char c = static_cast<char>(_getch());
            if (c == 27) running = false;  // ESC key
        }

        // Calculate processing time and add delay if needed
        auto frame_end = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(frame_end - frame_start).count();
        
        if (duration < FRAME_TIME) {
            auto sleep_time = static_cast<DWORD>((FRAME_TIME - duration) / 2);
            if (sleep_time > 0) {
                Sleep(sleep_time); // Use half the available time for better timing
            }
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
