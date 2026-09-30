#pragma once

namespace miximus::utils {

// Reinstall after SDK initialization so the application owns shutdown requests.
void install_shutdown_signal_handlers();
bool shutdown_requested() noexcept;

} // namespace miximus::utils
