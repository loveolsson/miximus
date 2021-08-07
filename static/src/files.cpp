
#ifdef _WIN32
#define LIBRARY_EXPORTS
#endif

#include "static_files/files.hpp"
#include <gzip/decompress.hpp>

namespace miximus::static_files {

std::string file_record::raw() const
{
    return gzip::decompress(reinterpret_cast<const char*>(gzipped.data()), gzipped.size());
}
} // namespace miximus::static_files