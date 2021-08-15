#pragma once
#include "wrapper/decklink-sdk/decklink_ptr.hpp"

#include <map>
#include <shared_mutex>
#include <string>
#include <vector>

struct IDeckLink;
struct IDeckLinkInput;
struct IDeckLinkOutput;
struct IDeckLinkDiscovery;
struct IDeckLinkVideoConversion;
struct IDeckLinkDisplayMode;

namespace miximus::nodes::decklink {

class discovery_callback;

class decklink_registry_s
{
    decklink_ptr<IDeckLinkDiscovery>    discovery_;
    std::unique_ptr<discovery_callback> callback_;

    std::shared_mutex                                    device_mutex_;
    std::map<IDeckLink*, std::string>                    names_;
    std::map<std::string, decklink_ptr<IDeckLinkInput>>  inputs_;
    std::map<std::string, decklink_ptr<IDeckLinkOutput>> outputs_;

    friend class discovery_callback;

  public:
    decklink_registry_s();
    ~decklink_registry_s();

    void uninstall();

    decklink_ptr<IDeckLinkInput>  get_input(const std::string& name);
    decklink_ptr<IDeckLinkOutput> get_output(const std::string& name);

    std::vector<std::string> get_input_names();
    std::vector<std::string> get_output_names();

    static std::string                            get_display_mode_name(IDeckLinkDisplayMode* mode);
    static decklink_ptr<IDeckLinkVideoConversion> get_converter();

    static std::unique_ptr<decklink_registry_s> create_decklink_registry();
};

} // namespace miximus::nodes::decklink