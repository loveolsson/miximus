#include "utils/file_dialog.hpp"

namespace miximus::utils {
struct file_dialog_s::impl_s
{
};
file_dialog_s::file_dialog_s()  = default;
file_dialog_s::~file_dialog_s() = default;
bool    file_dialog_s::supported() { return false; }
error_e file_dialog_s::open(bool, std::string, completion_t) { return error_e::unavailable; }
void    file_dialog_s::stop() {}
} // namespace miximus::utils
