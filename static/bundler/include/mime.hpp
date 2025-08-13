#pragma once
#include <boost/algorithm/string.hpp>
#include <frozen/map.h>

#include <array>
#include <filesystem>
#include <string_view>

static std::string_view get_mime(const std::filesystem::path& name)
{
    // Stripped version of NGINX default types, with charset for applicable types
    constexpr auto mime_types = frozen::make_map<std::string_view, std::string_view>({
        {".html",    "text/html;charset=UTF-8"                 },
        {".htm",     "text/html;charset=UTF-8"                 },
        {".shtml",   "text/html;charset=UTF-8"                 },
        {".css",     "text/css;charset=UTF-8"                  },
        {".xml",     "text/xml;charset=UTF-8"                  },
        {".gif",     "image/gif"                               },
        {".jpeg",    "image/jpeg"                              },
        {".jpg",     "image/jpeg"                              },
        {".js",      "application/javascript;charset=UTF-8"    },
        {".atom",    "application/atom+xml;charset=UTF-8"      },
        {".rss",     "application/rss+xml;charset=UTF-8"       },
        {".mml",     "text/mathml;charset=UTF-8"               },
        {".glsl",    "text/plain;charset=UTF-8"                },
        {".txt",     "text/plain;charset=UTF-8"                },
        {".md",      "text/plain;charset=UTF-8"                },
        {".jad",     "text/vnd.sun.j2me.app-descriptor"        },
        {".wml",     "text/vnd.wap.wml"                        },
        {".htc",     "text/x-component"                        },
        {".avif",    "image/avif"                              },
        {".png",     "image/png"                               },
        {".svg",     "image/svg+xml;charset=UTF-8"             },
        {".svgz",    "image/svg+xml;charset=UTF-8"             },
        {".tif",     "image/tiff"                              },
        {".tiff",    "image/tiff"                              },
        {".wbmp",    "image/vnd.wap.wbmp"                      },
        {".webp",    "image/webp"                              },
        {".ico",     "image/x-icon"                            },
        {".jng",     "image/x-jng"                             },
        {".bmp",     "image/x-ms-bmp"                          },
        {".woff",    "font/woff"                               },
        {".woff2",   "font/woff2"                              },
        {".jar",     "application/java-archive"                },
        {".war",     "application/java-archive"                },
        {".ear",     "application/java-archive"                },
        {".json",    "application/json;charset=UTF-8"          },
        {".hqx",     "application/mac-binhex40"                },
        {".doc",     "application/msword"                      },
        {".pdf",     "application/pdf"                         },
        {".ps",      "application/postscript"                  },
        {".eps",     "application/postscript"                  },
        {".ai",      "application/postscript"                  },
        {".rtf",     "application/rtf"                         },
        {".m3u8",    "application/vnd.apple.mpegurl"           },
        {".kml",     "application/vnd.google-earth.kml+xml"    },
        {".kmz",     "application/vnd.google-earth.kmz"        },
        {".xls",     "application/vnd.ms-excel"                },
        {".eot",     "application/vnd.ms-fontobject"           },
        {".ppt",     "application/vnd.ms-powerpoint"           },
        {".wmlc",    "application/vnd.wap.wmlc"                },
        {".wasm",    "application/wasm"                        },
        {".7z",      "application/x-7z-compressed"             },
        {".cco",     "application/x-cocoa"                     },
        {".jardiff", "application/x-java-archive-diff"         },
        {".jnlp",    "application/x-java-jnlp-file"            },
        {".run",     "application/x-makeself"                  },
        {".sit",     "application/x-stuffit"                   },
        {".der",     "application/x-x509-ca-cert"              },
        {".pem",     "application/x-x509-ca-cert"              },
        {".crt",     "application/x-x509-ca-cert"              },
        {".xpi",     "application/x-xpinstall"                 },
        {".xhtml",   "application/xhtml+xml;charset=UTF-8"     },
        {".xspf",    "application/xspf+xmlapplication/xspf+xml"},
        {".zip",     "application/zip"                         },
        {".mid",     "audio/midi"                              },
        {".midi",    "audio/midi"                              },
        {".kar",     "audio/midi"                              },
        {".mp3",     "audio/mpeg"                              },
        {".ogg",     "audio/ogg"                               },
        {".m4a",     "audio/x-m4a"                             },
        {".ra",      "audio/x-realaudio"                       },
        {".3gpp",    "video/3gpp"                              },
        {".3gp",     "video/3gpp"                              },
        {".ts",      "video/mp2t"                              },
        {".mp4",     "video/mp4"                               },
        {".mpeg",    "video/mpeg"                              },
        {".mpg",     "video/mpeg"                              },
        {".mov",     "video/quicktime"                         },
        {".webm",    "video/webm"                              },
        {".flv",     "video/x-flv"                             },
        {".m4v",     "video/x-m4v"                             },
        {".mng",     "video/x-mng"                             },
        {".asx",     "video/x-ms-asf"                          },
        {".asf",     "video/x-ms-asf"                          },
        {".wmv",     "video/x-ms-wmv"                          },
        {".avi",     "video/x-msvideo"                         },
    });

    auto ext = name.extension().string();
    boost::to_lower(ext);

    const auto it = mime_types.find(ext);
    if (it != mime_types.end()) {
        return it->second;
    }

    return "application/octet-stream";
}