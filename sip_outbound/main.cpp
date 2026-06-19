#include "call_manager.hpp"
#include <iostream>

int main() {
    try {
        auto& manager = CallManager::instance();
        std::cout << "Starting SIP server on port 8086..." << std::endl;
        manager.start();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
