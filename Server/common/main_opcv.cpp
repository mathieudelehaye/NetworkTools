#include <opencv2/opencv.hpp>
#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#define PORT 12345
#define CLIENT_IP "127.0.0.1"

int main() {
    // Init Winsock
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // Create socket
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    sockaddr_in clientAddr;
    clientAddr.sin_family = AF_INET;
    clientAddr.sin_port = htons(PORT);
    inet_pton(AF_INET, CLIENT_IP, &clientAddr.sin_addr);

    // Open webcam
    cv::VideoCapture cap(0);
    if (!cap.isOpened()) {
        std::cerr << "❌ Could not open webcam.\n";
        return -1;
    }

    std::vector<uchar> buffer;
    std::vector<int> params = { cv::IMWRITE_JPEG_QUALITY, 70 };

    cv::Mat frame;
    while (true) {
        cap >> frame;
        if (frame.empty()) continue;

        // Compress frame to JPEG
        cv::imencode(".jpg", frame, buffer, params);

        if (buffer.size() > 65000) {
            std::cerr << "⚠️ Frame too large for UDP. Skipping.\n";
            continue;
        }

        // Send JPEG data
        sendto(sock, reinterpret_cast<char*>(buffer.data()), buffer.size(), 0,
            reinterpret_cast<sockaddr*>(&clientAddr), sizeof(clientAddr));

        cv::waitKey(30); // ~33 FPS
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
