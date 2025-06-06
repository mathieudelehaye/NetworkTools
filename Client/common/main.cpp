#include <iostream>  
#ifdef _WIN32  
#include <winsock2.h>  
#include <ws2tcpip.h>  
#pragma comment(lib, "ws2_32.lib")  
#else  
#include <sys/socket.h>  
#include <netinet/in.h>  
#include <arpa/inet.h>  
#endif  

int main() {  
    int sockfd, newSockfd;

#ifdef _WIN32  
    WSADATA wsaData;  
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {  
        std::cout << "WSAStartup failed" << std::endl;  
        return 1;  
    }  
#endif  

    // Create a socket  
    sockfd = socket(AF_INET, SOCK_STREAM, 0);  

    if (sockfd == -1) {  
        std::cout << "Failed to create socket" << std::endl;  
#ifdef _WIN32  
        WSACleanup();  
#endif  
        return 1;  
    }  

    std::cout << "Socket created successfully" << std::endl;  

    // Set up the server address
    struct sockaddr_in serverAddr;

    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serverAddr.sin_port = htons(8081);

    // Bind the socket
    int bindResult = bind(sockfd, (struct sockaddr*)&serverAddr, sizeof(serverAddr));

    if (bindResult == -1) {
        std::cout << "Failed to bind socket" << std::endl;
        return 1;
    }

    std::cout << "Socket bound successfully" << std::endl;

    // Listen for connections
    int listenResult = listen(sockfd, 10);

    if (listenResult == -1) {
        std::cout << "Failed to listen for connections" << std::endl;
        return 1;
    }

    // Accept incoming connections
    struct sockaddr_in clientAddr;
    int clientAddrSize = sizeof(clientAddr);
    newSockfd = accept(sockfd, (struct sockaddr*)&clientAddr, &clientAddrSize);
    if (newSockfd == -1) {
        std::cout << "Failed to accept connection" << std::endl;
        return 1;
    }

    std::cout << "Accepted connection from client" << std::endl;


#ifdef _WIN32  
    closesocket(sockfd);  
    WSACleanup();  
#else  
    close(sockfd);  
#endif  

    return 0;  
}