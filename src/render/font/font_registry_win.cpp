#include "font_registry.hpp"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <Shlobj.h>
#include <Windows.h>
#include <string_view>
#include <vector>

namespace miximus::render {

struct init_data_s
{
    HDC                                                hdc;
    std::map<std::string, font_variant_s, std::less<>> files;
    std::map<std::string, font_info_s, std::less<>>    fonts;
    font_info_s*                                       font;
};

static std::string wchar_to_string(std::wstring_view wstr)
{
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), wstr.size(), NULL, 0, NULL, NULL);
    if (size <= 0) {
        return "";
    }

    std::string str(size, '\0');

    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), wstr.size(), str.data(), size, NULL, NULL);

    if (!str.empty() && str.front() == '@') {
        return str.substr(1);
    }

    return str;
}

static std::string fonts_path()
{
    wchar_t str[MAX_PATH] = {};
    SHGetSpecialFolderPathW(0, str, CSIDL_FONTS, FALSE);
    auto res = wchar_to_string(str);
    res += '\\';
    return res;
}

static void read_registry_fonts(init_data_s* data)
{
    static const LPWSTR fontRegistryPath = L"Software\\Microsoft\\Windows NT\\CurrentVersion\\Fonts";
    HKEY                hKey;
    LONG                result;
    std::wstring        res;

    // Open Windows font registry key
    result = RegOpenKeyExW(HKEY_LOCAL_MACHINE, fontRegistryPath, 0, KEY_READ, &hKey);
    if (result != ERROR_SUCCESS) {
        return;
    }

    DWORD maxValueNameSize, maxValueDataSize;
    result = RegQueryInfoKeyW(hKey, 0, 0, 0, 0, 0, 0, 0, &maxValueNameSize, &maxValueDataSize, 0, 0);
    if (result != ERROR_SUCCESS) {
        RegCloseKey(hKey);
        return;
    }

    DWORD                valueIndex = 0;
    std::vector<WCHAR>   valueName(maxValueNameSize + 1); // +1 for null terminator (not counted by RegQueryInfoKeyW)
    std::vector<uint8_t> valueData(maxValueDataSize);
    DWORD                valueNameSize, valueDataSize, valueType;

    auto path = fonts_path();

    do {
        valueDataSize = maxValueDataSize;
        valueNameSize = maxValueNameSize;

        result = RegEnumValueW(
            hKey, valueIndex, valueName.data(), &valueNameSize, 0, &valueType, valueData.data(), &valueDataSize);

        valueIndex++;

        if (result != ERROR_SUCCESS || valueType != REG_SZ) {
            continue;
        }

        std::wstring_view wsValueName(valueName.data(), valueNameSize);
        std::wstring_view wsValueData(reinterpret_cast<const wchar_t*>(valueData.data()),
                                      valueDataSize / sizeof(WCHAR) - 1);

        auto end_pos = wsValueName.find(L" (");
        if (end_pos == std::wstring_view::npos) {
            continue;
        }

        wsValueName = wsValueName.substr(0, end_pos);

        int i = 0;

        do {
            end_pos = wsValueName.find(L" & ");

            auto val = wsValueName.substr(0, end_pos);

            font_variant_s v{
                .index = i++,
                .path  = path + wchar_to_string(wsValueData),
            };

            data->files.emplace(wchar_to_string(val), std::move(v));

            if (end_pos != std::wstring_view::npos) {
                wsValueName = wsValueName.substr(end_pos + 3);
            }

        } while (end_pos != std::wstring_view::npos);

    } while (result != ERROR_NO_MORE_ITEMS);

    RegCloseKey(hKey);
}

static int CALLBACK font_enum_style_callback(const LOGFONTW*    lpelfe,
                                             const TEXTMETRICW* lpntme,
                                             DWORD              FontType,
                                             LPARAM             lParam)
{
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

static int CALLBACK font_enum_callback(const LOGFONTW* lpelfe, const TEXTMETRICW* lpntme, DWORD FontType, LPARAM lParam)
{
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

font_registry_s::font_map_t font_registry_s::scan_fonts()
{
    init_data_s data = {};
    read_registry_fonts(&data);

    data.hdc = GetDC(nullptr);

    if (data.hdc == nullptr) {
        return {};
    }

    EnumFontFamiliesExW(data.hdc, NULL, font_enum_callback, (LPARAM)&data, 0);
    ReleaseDC(nullptr, data.hdc);

    return std::move(data.fonts);
}

} // namespace miximus::render

#endif // _WIN32
