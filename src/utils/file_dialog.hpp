#pragma once
#include "types/error.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace miximus::utils {

struct file_dialog_result_s
{
    error_e                    error{error_e::no_error};
    std::optional<std::string> path;
};

// One outstanding dialog, owned by a dedicated Windows STA thread.
class file_dialog_s
{
    struct impl_s;
    std::unique_ptr<impl_s> impl_;

  public:
    using completion_t = std::function<void(file_dialog_result_s)>;
    file_dialog_s();
    ~file_dialog_s();
    file_dialog_s(const file_dialog_s&)            = delete;
    file_dialog_s& operator=(const file_dialog_s&) = delete;
    static bool    supported();
    error_e        open(bool image, std::string initial_path, completion_t completion);
    void           stop();
};

} // namespace miximus::utils
