#include "vertex_array.hpp"

namespace miximus::gpu {

vertex_array_s::vertex_array_s() { glGenVertexArrays(1, &id_); }

vertex_array_s::~vertex_array_s() { glDeleteVertexArrays(1, &id_); }

void vertex_array_s::bind() const { glBindVertexArray(id_); }

void vertex_array_s::unbind() { glBindVertexArray(0); }

} // namespace miximus::gpu
