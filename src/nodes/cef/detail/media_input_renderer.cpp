#include "media_input_renderer.hpp"

#include "wrapper/cef/media_input_abi.hpp"
#include "wrapper/cef/platform.hpp"

namespace miximus::nodes::cef::detail {
void install_media_inputs(const CefRefPtr<CefFrame>&     frame,
                          const CefRefPtr<CefV8Context>& context,
                          const std::string&             token,
                          uint32_t                       depth)
{
    const auto install = cef_wrapper::find_install_media_inputs();
    if (depth < cef_input_buffer_limits_s::MINIMUM_FRAME_COUNT ||
        depth > cef_input_buffer_limits_s::MAXIMUM_FRAME_COUNT) {
        return;
    }

    if (install == nullptr || cef_wrapper::find_send_media_frame() == nullptr || install(token.c_str(), depth) == 0) {
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
  const tracks = new Array(8);
  const api = globalThis.miximus || {};
  Object.defineProperty(api, "getInputMediaStream", {
    value: async (options) => {
      const index = options?.inputIndex;
      if (!Number.isInteger(index) || index < 0 || index >= 8) {
        throw new RangeError("inputIndex must be an integer in [0, 7]");
      }

      let stream = streams[index]?.deref();
      const track = tracks[index]?.deref();
      if (
        !stream ||
        !track ||
        track.readyState === "ended" ||
        stream.getTracks().length !== 1 ||
        stream.getVideoTracks()[0] !== track
      ) {
        const inputTrack = create(index);
        stream = new MediaStream([inputTrack]);
        tracks[index] = new WeakRef(inputTrack);
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
}

} // namespace miximus::nodes::cef::detail
