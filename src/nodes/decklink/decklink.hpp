#pragma once
#include "decklink_ptr.hpp"

#include <string>
#include <vector>

namespace miximus::nodes::decklink {

decklink_ptr<IDeckLinkIterator> get_device_iterator();

decklink_ptr<IDeckLink> get_device_by_index(int i);

std::vector<std::string> get_device_names();

void log_device_names();

} // namespace miximus::nodes::decklink