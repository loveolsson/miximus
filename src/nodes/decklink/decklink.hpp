#pragma once
#include "decklink_ptr.hpp"

#include <map>
#include <shared_mutex>
#include <string>
#include <vector>

namespace miximus::nodes::decklink {

class decklink_registry_s : public IDeckLinkDeviceNotificationCallback
{
    decklink_ptr<IDeckLinkDiscovery> discovery_;
    std::shared_mutex                device_mutex_;

    std::map<IDeckLink*, std::string>                    names_;
    std::map<std::string, decklink_ptr<IDeckLinkInput>>  inputs_;
    std::map<std::string, decklink_ptr<IDeckLinkOutput>> outputs_;

  public:
    decklink_registry_s();
    ~decklink_registry_s();

    HRESULT DeckLinkDeviceArrived(IDeckLink* deckLinkDevice) final;
    HRESULT DeckLinkDeviceRemoved(IDeckLink* deckLinkDevice) final;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID* ppv) final { return E_NOTIMPL; }
    ULONG                     AddRef() final { return 1; }
    ULONG                     Release() final { return 1; }

    decklink_ptr<IDeckLinkInput>  get_input(const std::string& name);
    decklink_ptr<IDeckLinkOutput> get_output(const std::string& name);

    std::vector<std::string> get_input_names();
    std::vector<std::string> get_output_names();
};

} // namespace miximus::nodes::decklink