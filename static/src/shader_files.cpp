#include "static_files/files.hpp"
#include <gzip/decompress.hpp>

namespace miximus::static_files {
// File: dummy.glsl (0 B / 20 B compressed)
	static const uint8_t fileData0[] = {
		0x1f, 0x8b, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
};

const file_map_t &get_shader_files() {
	static const file_map_t files{
		// File: dummy.glsl (0 B / 20 B compressed)
		{ "dummy.glsl", {
				{(const char*)fileData0, 20},
				gzip::decompress((const char*)fileData0, 20),
				"false"
			}
		},
	};
	return files;
};
} // namespace miximus::static_files
