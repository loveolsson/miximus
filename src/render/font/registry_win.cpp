#include "registry.hpp"

#define WIN32_LEAN_AND_MEAN
#include <Shlobj.h>
#include <Windows.h>

#include <string_view>

namespace miximus::render::font {

struct init_data_s
{
    HDC                                   hdc;
    std::map<std::string, font_variant_s> files;
    std::map<std::string, font_info_s>    fonts;
    font_info_s*                          font;
};

static std::string wchar_to_string(std::wstring_view wstr)
{
    int size = WideCharToMultiByte(CP_UTF8, 0, wstr.data(), wstr.size(), NULL, 0, NULL, NULL);
    if (size < 0) {
        return "";
    }

    auto* str = new char[(size_t)size + 1];

    WideCharToMultiByte(CP_UTF8, 0, wstr.data(), wstr.size(), str, size, NULL, NULL);
    str[size] = 0;

    auto* start = str;
    if (start[0] == '@') {
        start++;
    }

    std::string res(start);
    delete[] str;

    return res;
}

static std::string fonts_path()
{
    auto str = new wchar_t[MAX_PATH];

    SHGetSpecialFolderPathW(0, str, CSIDL_FONTS, FALSE);

    auto res(wchar_to_string(str));
    res += '\\';

    delete[] str;

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
        return;
    }

    DWORD  valueIndex = 0;
    LPWSTR valueName  = new WCHAR[maxValueNameSize];
    LPBYTE valueData  = new uint8_t[maxValueDataSize];
    DWORD  valueNameSize, valueDataSize, valueType;

    auto path = fonts_path();

    do {
        valueDataSize = maxValueDataSize;
        valueNameSize = maxValueNameSize;

        result = RegEnumValueW(hKey, valueIndex, valueName, &valueNameSize, 0, &valueType, valueData, &valueDataSize);

        valueIndex++;

        if (result != ERROR_SUCCESS || valueType != REG_SZ) {
            continue;
        }

        std::wstring_view wsValueName(valueName);
        std::wstring_view wsValueData((wchar_t*)valueData);

        auto end_pos = wsValueName.find(L" (");
        if (end_pos == std::wstring_view::npos) {
            continue;
        }

        wsValueName = wsValueName.substr(0, end_pos);

        int i = 0;

        do {
            end_pos = wsValueName.find(L" & ");

            auto val = wsValueName.substr(0, end_pos);

            font_variant_s v = {};
            v.index          = i++;
            v.path           = path + wchar_to_string(wsValueData);

            data->files.emplace(wchar_to_string(val), std::move(v));

            if (end_pos != std::wstring_view::npos) {
                wsValueName = wsValueName.substr(end_pos + 3);
            }

        } while (end_pos != std::wstring_view::npos);

    } while (result != ERROR_NO_MORE_ITEMS);

    delete[] valueName;
    delete[] valueData;

    RegCloseKey(hKey);
}

static int CALLBACK font_enum_style_callback(const LOGFONTW*    lpelfe,
                                             const TEXTMETRICW* lpntme,
                                             DWORD              FontType,
                                             LPARAM             lParam)
{
    auto* data = reinterpret_cast<init_data_s*>(lParam);
    auto* info = reinterpret_cast<const ENUMLOGFONTEXW*>(lpelfe);

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
    auto* data = reinterpret_cast<init_data_s*>(lParam);
    auto  name = wchar_to_string(lpelfe->lfFaceName);

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

font_registry_s::font_registry_s()
{
    init_data_s data = {};
    read_registry_fonts(&data);

    data.hdc = GetDC(nullptr);

    if (data.hdc == nullptr) {
        return;
    }

    EnumFontFamiliesExW(data.hdc, NULL, font_enum_callback, (LPARAM)&data, 0);

    fonts_ = std::move(data.fonts);
    log_fonts();
}

} // namespace miximus::render::font