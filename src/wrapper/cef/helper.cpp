#include "include/cef_app.h"

int main(int argc, char* argv[])
{
    // Ordinary Chromium child entry point. No Miximus services or GPU setup.
    const CefMainArgs args(argc, argv);
    return CefExecuteProcess(args, nullptr, nullptr);
}
