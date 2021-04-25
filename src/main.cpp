#include "static_files/files.hpp"
#include <websocketpp/version.hpp>

#include <asio.hpp>
#include <iostream>

int main() {
  using namespace miximus;

  auto &files = static_files::webFiles;

  std::cout << "Files" << std::endl;
  for (auto &file : files) {
    std::cout << "File: " << file.first << std::endl;
  }

  return 0;
}