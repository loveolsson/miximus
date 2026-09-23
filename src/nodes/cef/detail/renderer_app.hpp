#pragma once

#include "include/cef_app.h"

namespace miximus::nodes::cef::detail {
// Ordinary CEF helper app. Only renderer/V8 command handling lives here;
// no Miximus graph, GPU device, service or public web control API.
CefRefPtr<CefApp> create_renderer_app();
} // namespace miximus::nodes::cef::detail
