#pragma once

namespace miximus::gpu::detail {

// Owns a native resource reference, not GPU access or completion.
class native_handle_s
{
  public:
#ifdef _WIN32
    using value_t                    = void*;
    static constexpr value_t invalid = nullptr;
#else
    using value_t                    = int;
    static constexpr value_t invalid = -1;
#endif

  private:
    value_t value_{invalid};

  public:
    explicit native_handle_s(value_t value = invalid) noexcept
        : value_(value)
    {
    }

    ~native_handle_s();

    native_handle_s(const native_handle_s&)            = delete;
    native_handle_s& operator=(const native_handle_s&) = delete;

    native_handle_s(native_handle_s&& other) noexcept;
    native_handle_s& operator=(native_handle_s&& other) noexcept;

    value_t get() const noexcept { return value_; }

    [[nodiscard]] value_t release() noexcept;
    void                  reset(value_t value = invalid) noexcept;

    static native_handle_s duplicate(value_t borrowed);
};

} // namespace miximus::gpu::detail
