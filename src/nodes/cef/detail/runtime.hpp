#pragma once

#include <filesystem>
#include <memory>

namespace miximus::nodes::cef::detail {

// CEF headers stay behind this implementation boundary. Wrapper staging keeps
// bundled Vulkan/ANGLE libraries out of the main executable's loader search.
class runtime_s
{
    struct state_s;
    std::unique_ptr<state_s> state_;

  public:
    // Construct and destroy on the same startup/shutdown thread. CEF owns its
    // supported threaded UI loop; the application never pumps browser events.
    runtime_s(const std::filesystem::path& runtime_directory, const std::filesystem::path& profile_directory);
    ~runtime_s();
    runtime_s(const runtime_s& other)            = delete;
    runtime_s& operator=(const runtime_s& other) = delete;
};

} // namespace miximus::nodes::cef::detail
