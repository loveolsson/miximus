#pragma once

#include "include/cef_app.h"
#include "media_input_abi.hpp"

#include <filesystem>

namespace miximus::cef_wrapper {

std::filesystem::path  runtime_directory();
std::filesystem::path  vulkan_loader_path();
install_media_inputs_t find_install_media_inputs();
send_media_frame_t     find_send_media_frame();
void                   load_runtime(const std::filesystem::path& directory);
int                    execute_subprocess(int argc, char** argv, const CefRefPtr<CefApp>& app);

} // namespace miximus::cef_wrapper
