#include <opencv2/opencv.hpp>
#include <iostream>
#include <winsock2.h>
#include <ws2tcpip.h>

#pragma comment(lib, "ws2_32.lib")

#define PORT 12345
#define BUFFER_SIZE 65536

int main() {
    // Init Winsock
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    // Create socket
    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    sockaddr_in serverAddr;
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(PORT);
    serverAddr.sin_addr.s_addr = INADDR_ANY;

    bind(sock, reinterpret_cast<sockaddr*>(&serverAddr), sizeof(serverAddr));

    char buffer[BUFFER_SIZE];
    sockaddr_in senderAddr;
    int senderLen = sizeof(senderAddr);

    while (true) {
        int bytesReceived = recvfrom(sock, buffer, BUFFER_SIZE, 0,
            reinterpret_cast<sockaddr*>(&senderAddr), &senderLen);

        if (bytesReceived > 0) {
            std::vector<uchar> data(buffer, buffer + bytesReceived);
            cv::Mat img = cv::imdecode(data, cv::IMREAD_COLOR);

            if (!img.empty()) {
                cv::imshow("Received Stream", img);
            }
        }

        if (cv::waitKey(1) == 27) break; // ESC to quit
    }

    closesocket(sock);
    WSACleanup();
    return 0;
}
