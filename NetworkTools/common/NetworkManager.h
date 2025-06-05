#pragma once

#include <string>

namespace NetworkTools {

class NetworkManager {
public:
    NetworkManager();
    ~NetworkManager();

    void initialize();
    bool isConnected() const;
};

std::string getVersion();

}
