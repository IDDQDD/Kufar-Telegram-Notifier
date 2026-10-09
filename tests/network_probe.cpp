#include "networking.hpp"
#include <iostream>
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    try { Networking::getJSONFromURL(argv[1]); return 0; }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
