#include <iostream>
#include <string>
#include <limits>
#include <queue>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <opencv2/opencv.hpp>
#include <map>
#include <fstream>
// Fix for Windows max macro conflict
#define NOMINMAX
#ifdef _WIN32
#include <conio.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <direct.h>  // for _getcwd
#pragma comment(lib, "ws2_32.lib")
#endif
#include "../include/tcp_client.h"
#include "../include/udp_client.h"

#define VIDEO_PORT 12345
#define BUFFER_SIZE 262144  // Increased to 256KB

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

// Frame reassembly structure
struct FrameChunk {
    std::vector<char> data;
    bool received{false};  // Initialize to false
};

struct Frame {
    std::map<uint32_t, FrameChunk> chunks;
    size_t total_size{0};  // Initialize to 0
    std::chrono::steady_clock::time_point timestamp;
};

bool run_udp_video_demo(const char* server_ip) noexcept {
    try {
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
        serverAddr.sin_addr.s_addr = INADDR_ANY;  // Listen on all interfaces

        if (bind(sock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr)) == SOCKET_ERROR) {
            std::cerr << "Failed to bind socket: " << WSAGetLastError() << std::endl;
            closesocket(sock);
            WSACleanup();
            return false;
        }

        // Print binding information
        char host[256];
        if (gethostname(host, sizeof(host)) == 0) {
            struct addrinfo hints, *res;
            memset(&hints, 0, sizeof(hints));
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_DGRAM;

            if (getaddrinfo(host, NULL, &hints, &res) == 0) {
                char ipstr[INET_ADDRSTRLEN];
                struct sockaddr_in *addr = (struct sockaddr_in *)res->ai_addr;
                inet_ntop(AF_INET, &(addr->sin_addr), ipstr, INET_ADDRSTRLEN);
                std::cout << "Client listening on " << ipstr << ":" << VIDEO_PORT << std::endl;
                freeaddrinfo(res);
            }
        }

        // Increase receive buffer size
        int rcvbuf = 262144;  // 256KB buffer
        if (setsockopt(sock, SOL_SOCKET, SO_RCVBUF, (char*)&rcvbuf, sizeof(rcvbuf)) < 0) {
            std::cerr << "Failed to set receive buffer size\n";
        }

        // Set socket to non-blocking mode
        u_long mode = 1;
        ioctlsocket(sock, FIONBIO, &mode);

        std::cout << "Receiving video stream. Press ESC to stop.\n";

        std::vector<char> buffer(BUFFER_SIZE);
        sockaddr_in senderAddr;
        int senderLen = sizeof(senderAddr);

        // Frame management
        std::map<uint32_t, std::vector<std::vector<uchar>>> frame_chunks;
        const size_t HEADER_SIZE = 12;  // 12 bytes: 4 for frame_id + 4 for chunk_id + 4 for total_chunks
        const auto FRAME_TIMEOUT = std::chrono::milliseconds(100); // Reduced timeout
        const size_t MAX_FRAME_QUEUE = 30; // Maximum frames to keep in memory

        // FPS calculation variables
        const int FPS_WINDOW_SIZE = 30;
        std::queue<std::chrono::steady_clock::time_point> frame_times;
        double current_fps = 0.0;

        // Debug variables
        size_t total_bytes_received = 0;
        size_t packets_received = 0;
        auto last_debug = std::chrono::steady_clock::now();
        uint32_t last_frame_id = 0;
        size_t received_chunks = 0;
        size_t completed_frames = 0;
        uint32_t last_displayed_frame = 0;
        bool test_frame_saved = false;  // Flag to ensure we only save one frame
        bool failed_frame_saved = false; // <--- Add this flag
        auto stream_start_time = std::chrono::steady_clock::now();

        // Create window with OpenCV high GUI
        cv::namedWindow("Video Stream", cv::WINDOW_AUTOSIZE | cv::WINDOW_GUI_NORMAL);

        bool running = true;
        while (running) {
            auto now = std::chrono::steady_clock::now();

            // Debug output every second
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_debug).count() >= 1) {
                std::cout << "Client stats - Received: " << total_bytes_received / 1024 << " KB, "
                         << "Packets: " << packets_received << ", "
                         << "Chunks: " << received_chunks << ", "
                         << "Completed frames: " << completed_frames << ", "
                         << "Current frame: " << last_frame_id 
                         << ", Last displayed: " << last_displayed_frame << std::endl;
                
                // Reset counters
                total_bytes_received = 0;
                packets_received = 0;
                received_chunks = 0;
                completed_frames = 0;
                last_debug = now;
            }

            // Receive chunks with timeout
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(sock, &readfds);
            
            timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 10000; // 10ms timeout
            
            int ready = select(0, &readfds, nullptr, nullptr, &tv);
            
            if (ready > 0) {
                int bytesReceived = recvfrom(sock, buffer.data(), static_cast<int>(buffer.size()), 0,
                    reinterpret_cast<sockaddr*>(&senderAddr), &senderLen);

                if (bytesReceived > static_cast<int>(HEADER_SIZE)) {
                    total_bytes_received += static_cast<size_t>(bytesReceived);
                    packets_received++;
                    received_chunks++;

                    // Extract header information
                    uint32_t frame_id = ntohl(*reinterpret_cast<uint32_t*>(buffer.data()));
                    uint32_t chunk_id = ntohl(*reinterpret_cast<uint32_t*>(buffer.data() + 4));
                    uint32_t total_chunks = ntohl(*reinterpret_cast<uint32_t*>(buffer.data() + 8)); // New: total_chunks from header
                    // Calculate chunk size from received data
                    size_t chunk_size = bytesReceived - HEADER_SIZE;

                    // Add bounds checking and debug output
                    std::cout << "Received packet - Frame: " << frame_id 
                            << ", Chunk: " << chunk_id 
                            << "/" << total_chunks
                            << ", Size: " << chunk_size << " bytes" << std::endl;

                    // Sanity check the values
                    const uint32_t MAX_CHUNKS = 100;  // Reasonable maximum number of chunks
                    const uint32_t MAX_CHUNK_SIZE = 1024 * 1024;  // 1MB max chunk size

                    if (chunk_size == 0 || chunk_size > MAX_CHUNK_SIZE || total_chunks == 0 || total_chunks > MAX_CHUNKS) {
                        std::cerr << "Invalid chunk_size or total_chunks value: " << chunk_size << ", " << total_chunks << std::endl;
                        continue;
                    }

                    // Create or get frame buffer
                    if (frame_chunks.find(frame_id) == frame_chunks.end()) {
                        if (chunk_id == 0) {
                            frame_chunks[frame_id] = std::vector<std::vector<uchar>>(total_chunks);
                            std::cout << "Created new frame buffer for frame " << frame_id 
                                    << " with " << total_chunks << " chunks" << std::endl;
                        } else {
                            std::cerr << "Received chunk " << chunk_id << " before chunk 0 for frame " << frame_id << std::endl;
                            continue;
                        }
                    }

                    // Store this chunk if within bounds
                    if (chunk_id < frame_chunks[frame_id].size()) {
                        frame_chunks[frame_id][chunk_id] = std::vector<uchar>(
                            buffer.begin() + HEADER_SIZE,
                            buffer.begin() + bytesReceived
                        );
                        std::cout << "Stored chunk " << chunk_id << " of frame " << frame_id 
                                << " (size: " << chunk_size << " bytes)" << std::endl;

                        // Check if we have all chunks for this frame
                        bool frame_complete = true;
                        size_t total_size = 0;
                        size_t received_chunks = 0;

                        // Count received chunks and calculate total size
                        for (size_t i = 0; i < frame_chunks[frame_id].size(); i++) {
                            if (!frame_chunks[frame_id][i].empty()) {
                                received_chunks++;
                                total_size += frame_chunks[frame_id][i].size();
                            } else {
                                frame_complete = false;
                            }
                        }

                        std::cout << "Frame " << frame_id << " status: " 
                                << received_chunks << "/" << frame_chunks[frame_id].size() 
                                << " chunks received" << std::endl;

                        // Only decode if all chunks are present
                        if (frame_complete && received_chunks > 0) {
                            // Combine all chunks into one frame
                            std::vector<uchar> frameData;
                            frameData.reserve(total_size);
                            for (const auto& chunk : frame_chunks[frame_id]) {
                                if (!chunk.empty()) {
                                    frameData.insert(frameData.end(), chunk.begin(), chunk.end());
                                }
                            }
                            try {
                                std::cout << "Attempting to decode frame " << frame_id 
                                        << " (total size: " << frameData.size() << " bytes from " 
                                        << frame_chunks[frame_id].size() << " chunks)" << std::endl;
                                // Debug: Check first few bytes of the data
                                std::cout << "First bytes: ";
                                for (size_t i = 0; i < std::min(size_t(16), frameData.size()); i++) {
                                    printf("%02X ", frameData[i]);
                                }
                                std::cout << std::endl;
                                // Check for JPEG end marker
                                bool hasEndMarker = false;
                                for (size_t i = frameData.size() - 2; i > 0; --i) {
                                    if (frameData[i] == 0xFF && frameData[i + 1] == 0xD9) {
                                        hasEndMarker = true;
                                        break;
                                    }
                                }
                                if (!hasEndMarker) {
                                    std::cerr << "Warning: Frame " << frame_id << " is missing JPEG end marker" << std::endl;
                                }
                                // Try to decode the frame
                                cv::Mat img = cv::imdecode(frameData, cv::IMREAD_COLOR);
                                if (img.empty()) {
                                    std::cerr << "Failed to decode frame " << frame_id << std::endl;
                                    // Save the failed frame data for debugging, but only once per session
                                    if (!failed_frame_saved) {
                                        std::string debug_filename = "failed_frame_" + std::to_string(frame_id) + ".jpg";
                                        std::ofstream debug_file(debug_filename, std::ios::binary);
                                        if (debug_file.is_open()) {
                                            debug_file.write(reinterpret_cast<const char*>(frameData.data()), frameData.size());
                                            std::cerr << "Saved failed frame data to " << debug_filename << std::endl;
                                            failed_frame_saved = true;
                                        }
                                    }
                                } else {
                                    // --- FPS calculation ---
                                    frame_times.push(std::chrono::steady_clock::now());
                                    while (frame_times.size() > FPS_WINDOW_SIZE) {
                                        frame_times.pop();
                                    }
                                    if (frame_times.size() >= 2) {
                                        auto time_diff = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            frame_times.back() - frame_times.front()).count();
                                        if (time_diff > 0) {
                                            current_fps = (frame_times.size() - 1) * 1000.0 / time_diff;
                                        }
                                    }
                                    // Overlay resolution and FPS
                                    std::stringstream info;
                                    info << "Resolution: " << img.cols << "x" << img.rows
                                         << " | FPS: " << std::fixed << std::setprecision(1) << current_fps;
                                    cv::putText(img, info.str(), cv::Point(10, 30),
                                        cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 255, 0), 2);
                                    std::cout << "Successfully decoded frame " << frame_id 
                                            << " (" << img.cols << "x" << img.rows << ")" << std::endl;
                                    
                                    // Save one test frame after 3 seconds of streaming
                                    auto now = std::chrono::steady_clock::now();
                                    auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - stream_start_time).count();
                                    
                                    #ifdef DEBUG_SAVE_TEST_FRAME
                                    if (!test_frame_saved && elapsed >= 3) {
                                        std::cout << "Attempting to save test frame " << frame_id 
                                                << " (elapsed time: " << elapsed << "s)" << std::endl;
                                        // First try to save in current directory
                                        std::string filename = "test_frame_" + std::to_string(frame_id) + ".jpg";
                                        std::cout << "Attempting to save to current directory: " << filename << std::endl;
                                        std::vector<int> compression_params;
                                        compression_params.push_back(cv::IMWRITE_JPEG_QUALITY);
                                        compression_params.push_back(95);
                                        bool saved = false;
                                        try {
                                            if (cv::imwrite(filename, img, compression_params)) {
                                                std::cout << "Successfully saved test frame to current directory" << std::endl;
                                                saved = true;
                                            } else {
                                                std::cerr << "Failed to save to current directory" << std::endl;
                                            }
                                        } catch (const cv::Exception& e) {
                                            std::cerr << "OpenCV exception while saving frame: " << e.what() << std::endl;
                                        } catch (const std::exception& e) {
                                            std::cerr << "Standard exception while saving frame: " << e.what() << std::endl;
                                        }
                                        if (!saved) {
                                            /* Try to save in executable directory (Windows) */
                                            char current_path[MAX_PATH];
                                            if (GetModuleFileNameA(NULL, current_path, MAX_PATH) != 0) {
                                                std::string path(current_path);
                                                size_t last_slash = path.find_last_of("\\");
                                                if (last_slash != std::string::npos) {
                                                    path = path.substr(0, last_slash + 1);
                                                    filename = path + "test_frame_" + std::to_string(frame_id) + ".jpg";
                                                    std::cout << "Attempting to save to executable directory: " << filename << std::endl;
                                                    if (cv::imwrite(filename, img, compression_params)) {
                                                        std::cout << "Successfully saved test frame to executable directory" << std::endl;
                                                        saved = true;
                                                    } else {
                                                        std::cerr << "Failed to save to executable directory" << std::endl;
                                                    }
                                                }
                                            }
                                        }
                                        if (saved) {
                                            test_frame_saved = true;
                                        }
                                    }
                                    #endif

                                    // Display the frame
                                    cv::imshow("Video Stream", img);
                                    last_displayed_frame = frame_id;
                                }
                            } catch (const cv::Exception& e) {
                                std::cerr << "OpenCV exception while processing frame: " << e.what() << std::endl;
                            } catch (const std::exception& e) {
                                std::cerr << "Standard exception while processing frame: " << e.what() << std::endl;
                            }

                            // Clean up the chunks for this frame
                            frame_chunks.erase(frame_id);
                        }
                    }
                }
            }

            // Limit frame queue size
            while (frame_chunks.size() > MAX_FRAME_QUEUE) {
                frame_chunks.erase(frame_chunks.begin());
            }

            // Process window events and check for ESC key
            char c = static_cast<char>(cv::waitKey(1));
            if (c == 27) running = false;  // ESC key

            // Small delay to prevent CPU overuse
            Sleep(1);
        }

        cv::destroyAllWindows();
        closesocket(sock);
        WSACleanup();
        return true;
    }
    catch (const std::exception& e) {
        std::cerr << "Error in UDP video demo: " << e.what() << std::endl;
        return false;
    }
}

int main(int argc, char* argv[]) {
    // Allow optional server IP as first argument (default = localhost)
    const char* SERVER_IP = (argc > 1 ? argv[1] : "127.0.0.1");
    const uint16_t SERVER_PORT = 8080;

    // Silence INFO?level plugin?loader messages
    cv::utils::logging::setLogLevel(cv::utils::logging::LOG_LEVEL_WARNING);

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
