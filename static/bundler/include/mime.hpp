#include <array>
#include <boost/algorithm/string.hpp>
#include <filesystem>
#include <frozen/map.h>
#include <string_view>

static std::string_view get_mime(const std::filesystem::path& name)
{
    constexpr auto mimes = frozen::make_map<std::string_view, std::string_view>({
        {".css",  "text/css;charset=UTF-8"        },
        {".html", "text/html;charset=UTF-8"       },
        {".js",   "text/javascript;charset=UTF-8" },
        {".json", "application/json;charset=UTF-8"},
        {".glsl", "text/plain;charset=UTF-8"      },
        {".txt",  "text/plain;charset=UTF-8"      },
        {".md",   "text/plain;charset=UTF-8"      },
        {".jpg",  "image/jpeg"                    },
        {".jpeg", "image/jpeg"                    },
        {".png",  "image/png"                     },
        {".gif",  "image/gif"                     },
        {".webp", "image/webp"                    },
        {".svg",  "image/svg+xml"                 },
        {".ico",  "image/x-icon"                  },
    });

    auto ext = name.extension().string();
    boost::to_lower(ext);

    const auto it = mimes.find(ext);
    if (it != mimes.end()) {
        return it->second;
    }

    return "application/octet-stream";
}