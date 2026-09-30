#include "font_registry.hpp"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <Shlobj.h>
#include <Windows.h>
#include <array>
#include <filesystem>
#include <limits>
#include <string_view>
#include <vector>

namespace miximus::render {
namespace {

struct init_data_s
{
    HDC                                 hdc{};
    utils::string_map_t<font_variant_s> files;
    utils::string_map_t<font_info_s>    fonts;
    font_info_s*                        font{};
};

std::string wchar_to_string(std::wstring_view wstr)
{
    if (wstr.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        return {};
    }

    const int size =
        WideCharToMultiByte(CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return "";
    }

    std::string str(static_cast<size_t>(size), '\0');

    if (WideCharToMultiByte(
            CP_UTF8, 0, wstr.data(), static_cast<int>(wstr.size()), str.data(), size, nullptr, nullptr) != size) {
        return {};
    }

    if (!str.empty() && str.front() == '@') {
        return str.substr(1);
    }

    return str;
}

std::filesystem::path fonts_path()
{
    std::array<wchar_t, MAX_PATH> str{};
    if (SHGetSpecialFolderPathW(nullptr, str.data(), CSIDL_FONTS, FALSE) == FALSE) {
        return {};
    }

    return str.data();
}

void read_registry_fonts(init_data_s* data)
{
    static constexpr auto fontRegistryPath = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts";
    HKEY                  hKey{};

    // Open Windows font registry key
    auto result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, fontRegistryPath, 0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) {
        return;
    }

    DWORD maxValueNameSize{};
    DWORD maxValueDataSize{};
    result = RegQueryInfoKeyW(hKey,
                              nullptr,
                              nullptr,
                              nullptr,
                              nullptr,
                              nullptr,
                              nullptr,
                              nullptr,
                              &maxValueNameSize,
                              &maxValueDataSize,
                              nullptr,
                              nullptr);
    if (result != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return;
    }

    DWORD                valueIndex = 0;
    std::vector<WCHAR>   valueName(maxValueNameSize + 1); // +1 for null terminator (not counted by RegQueryInfoKeyW)
    std::vector<uint8_t> valueData(maxValueDataSize);
    DWORD                valueNameSize{};
    DWORD                valueDataSize{};
    DWORD                valueType{};

    auto path = fonts_path();

    while (result != ERROR_NO_MORE_ITEMS) {
        valueDataSize = maxValueDataSize;
        valueNameSize = static_cast<DWORD>(valueName.size());

        result = RegEnumValueW(
            hKey, valueIndex, valueName.data(), &valueNameSize, nullptr, &valueType, valueData.data(), &valueDataSize);

        valueIndex++;

        if (result != ERROR_SUCCESS || valueType != REG_SZ || valueDataSize < sizeof(WCHAR)) {
            continue;
        }

        std::wstring_view wsValueName(valueName.data(), valueNameSize);
        std::wstring_view wsValueData(reinterpret_cast<const wchar_t*>(valueData.data()),
                                      (valueDataSize / sizeof(WCHAR)) - 1);

        auto end_pos = wsValueName.find(L" (");
        if (end_pos == std::wstring_view::npos) {
            continue;
        }

        wsValueName = wsValueName.substr(0, end_pos);

        int i = 0;

        while (true) {
            end_pos = wsValueName.find(L" & ");

            auto val = wsValueName.substr(0, end_pos);

            font_variant_s v{
                .index = i++,
                .path  = path / std::filesystem::path(wsValueData),
            };

            data->files.emplace(wchar_to_string(val), std::move(v));

            if (end_pos == std::wstring_view::npos) {
                break;
            }

            wsValueName = wsValueName.substr(end_pos + 3);
        }
    }

    RegCloseKey(hKey);
}

int CALLBACK font_enum_style_callback(const LOGFONTW* lpelfe,
                                      const TEXTMETRICW* /* lpntme */,
                                      DWORD /* FontType */,
                                      LPARAM lParam)
{
    // Win32 passes the caller-owned context through LPARAM.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto data = reinterpret_cast<init_data_s*>(lParam);
    auto info = reinterpret_cast<const ENUMLOGFONTEXW*>(lpelfe);

    auto style     = wchar_to_string(info->elfStyle);
    auto full_name = wchar_to_string(info->elfFullName);

    auto it = data->files.find(full_name);
    if (it != data->files.end() && data->font != nullptr) {
        auto variant = it->second;
        variant.name = style;
        data->font->variants.emplace(style, std::move(variant));
    }

    return 1;
}

int CALLBACK font_enum_callback(const LOGFONTW* lpelfe,
                                const TEXTMETRICW* /* lpntme */,
                                DWORD /* FontType */,
                                LPARAM lParam)
{
    // Win32 passes the caller-owned context through LPARAM.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    auto data = reinterpret_cast<init_data_s*>(lParam);
    auto name = wchar_to_string(lpelfe->lfFaceName);

    font_info_s font;
    font.name  = name;
    data->font = &font;

    LOGFONTW d = *lpelfe;

    EnumFontFamiliesExW(data->hdc, &d, font_enum_style_callback, lParam, 0);

    if (!font.variants.empty()) {
        data->fonts.emplace(name, std::move(font));
    }

    data->font = nullptr;

    return 1;
}

} // namespace

font_registry_s::font_map_t font_registry_s::scan_fonts()
{
    init_data_s data = {};
    read_registry_fonts(&data);

    data.hdc = GetDC(nullptr);

    if (data.hdc == nullptr) {
        return {};
    }

    EnumFontFamiliesExW(data.hdc, nullptr, font_enum_callback, reinterpret_cast<LPARAM>(&data), 0);
    ReleaseDC(nullptr, data.hdc);

    return std::move(data.fonts);
}

} // namespace miximus::render

#endif // _WIN32
