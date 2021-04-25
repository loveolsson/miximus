#define BOOST_ASIO_STANDALONE 
#define BOOST_ASIO_HAS_STD_ADDRESSOF
#define BOOST_ASIO_HAS_STD_ARRAY
#define BOOST_ASIO_HAS_CSTDINT
#define BOOST_ASIO_HAS_STD_SHARED_PTR
#define BOOST_ASIO_HAS_STD_TYPE_TRAITS

#include "static_files/files.hpp"
#include <websocketpp/version.hpp>
#include <boost/asio.hpp>

#include <iostream>

int main() {
    using namespace miximus;
    auto& files = static_files::files;

    std::cout << "Files" << std::endl;
    for (auto& file : files) {
        std::cout <<"File: "<< file.first << std::endl;
    }

    return 0;
}