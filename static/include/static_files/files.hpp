#pragma once
#include <string>
#include <unordered_map>

namespace miximus::static_files {
struct file_record {
  std::string gzipped;
  std::string raw;
  std::string mime;
};

extern const std::unordered_map<std::string_view, file_record> web_files;
extern const std::unordered_map<std::string_view, file_record> shader_files;
} // namespace miximus::static_files
