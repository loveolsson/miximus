#include "include/cef_app.h"
#include "nodes/cef/detail/renderer_app.hpp"

int main(int argc, char* argv[])
{
    // Ordinary Chromium child entry point. No Miximus services or GPU setup.
    const CefMainArgs args(argc, argv);
    return CefExecuteProcess(args, miximus::nodes::cef::detail::create_renderer_app(), nullptr);
}
