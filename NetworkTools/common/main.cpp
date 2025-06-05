#include "library.h"
#include "NetworkManager.h"

#include <iostream>

int main() {
	std::cout << "Network Tools Library Version: " << NetworkTools::getVersion() << std::endl;
	
	// Example usage of the library
	NetworkTools::NetworkManager manager;
	manager.initialize();
	
	if (manager.isConnected()) {
		std::cout << "Network is connected." << std::endl;
	} else {	
		std::cout << "Network is not connected." << std::endl;
	}
	
	return 0;
}