#include "execution_context.hpp"

#include "frame_commands.hpp"

#include <utility>

namespace miximus::nodes {
const std::string& configuration_context_s::cef_error() const
{
    static const std::string unavailable{"CEF support is not available"};
    return browser_error != nullptr ? *browser_error : unavailable;
}

gpu::recording_s& execution_context_s::commands() { return commands_.commands(); }

gpu::completion_s execution_context_s::submit_gpu() { return commands_.submit_gpu(); }

void execution_context_s::defer_output(std::function<void(gpu::completion_s)> publish)
{
    commands_.defer_output(std::move(publish));
}

const std::string& execution_context_s::cef_error() const
{
    return configuration_context_s{.browser = services_.browser, .browser_error = services_.browser_error}.cef_error();
}
} // namespace miximus::nodes
