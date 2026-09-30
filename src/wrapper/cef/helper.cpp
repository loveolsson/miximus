#include "include/cef_app.h"
#include "nodes/cef/detail/renderer_app.hpp"
#include "platform.hpp"

int main(int argc, char* argv[])
{
    // Ordinary Chromium child entry point. No Miximus services or GPU setup.
    return miximus::cef_wrapper::execute_subprocess(argc, argv, miximus::nodes::cef::detail::create_renderer_app());
}
