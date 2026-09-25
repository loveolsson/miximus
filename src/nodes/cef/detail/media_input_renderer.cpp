#include "media_input_renderer.hpp"

#include "wrapper/cef/media_input_abi.hpp"

#include <charconv>
#include <cstdlib>
#include <string_view>

#ifdef __linux__
#include <dlfcn.h>
#endif

namespace miximus::nodes::cef::detail {
void install_media_inputs(const CefRefPtr<CefFrame>&     frame,
                          const CefRefPtr<CefV8Context>& context,
                          const std::string&             token)
{
#ifdef __linux__
    const auto install =
        reinterpret_cast<cef_wrapper::install_media_inputs_t>(dlsym(RTLD_DEFAULT, cef_wrapper::INSTALL_MEDIA_INPUTS));
    uint32_t depth = 3;
    // Diagnostic override only; page JavaScript cannot increase pool capacity.
    // The helper reads its immutable environment during renderer startup.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    if (const auto* configured = std::getenv("MIXIMUS_CEF_MEDIA_INPUT_DEPTH")) {
        const std::string_view value(configured);
        const auto             parsed = std::from_chars(value.data(), value.data() + value.size(), depth);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || depth < 1 || depth > 8) {
            return;
        }
    }

    if (install == nullptr || dlsym(RTLD_DEFAULT, cef_wrapper::SEND_MEDIA_FRAME) == nullptr ||
        install(token.c_str(), depth) == 0) {
        return;
    }

    (void)frame;
    auto global = context->GetGlobal();
    auto create = global->GetValue("__miximusCreateInputTrack");
    global->DeleteValue("__miximusCreateInputTrack");
    if (!create || !create->IsFunction()) {
        return;
    }

    constexpr auto*           script = R"JS(
(function (create) {
  const streams = new Array(8);
  const api = globalThis.miximus || {};
  Object.defineProperty(api, "getInputMediaStream", {
    value: async (options) => {
      const index = options?.inputIndex;
      if (!Number.isInteger(index) || index < 0 || index >= 8) {
        throw new RangeError("inputIndex must be an integer in [0, 7]");
      }

      let stream = streams[index]?.deref();
      if (!stream || stream.getVideoTracks()[0].readyState === "ended") {
        stream = new MediaStream([create(index)]);
        streams[index] = new WeakRef(stream);
      }

      return stream;
    },
    enumerable: true,
  });
  Object.defineProperty(globalThis, "miximus", {
    value: api,
    configurable: false,
  });
})
)JS";
    CefRefPtr<CefV8Value>     setup;
    CefRefPtr<CefV8Exception> exception;
    if (context->Eval(script, "miximus-media-input", 1, setup, exception)) {
        setup->ExecuteFunction(nullptr, {create});
    }

#else
    (void)frame;
    (void)context;
    (void)token;
#endif
}

} // namespace miximus::nodes::cef::detail
