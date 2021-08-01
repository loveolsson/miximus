#include "vertex_buffer.hpp"
#include "glad.hpp"

namespace miximus::gpu {

vertex_buffer_s::vertex_buffer_s() { glGenBuffers(1, &id_); }

vertex_buffer_s::~vertex_buffer_s() { glDeleteBuffers(1, &id_); }

void vertex_buffer_s::bind() const { glBindBuffer(GL_ARRAY_BUFFER, id_); }

void vertex_buffer_s::set_data(const void* data, size_t element_size, size_t count)
{
    size_t size   = element_size * count;
    vertex_count_ = count;

    glNamedBufferData(id_, size, data, GL_STATIC_DRAW);
}

void vertex_buffer_s::draw() const { glDrawArrays(GL_TRIANGLES, 0, vertex_count_); }

} // namespace miximus::gpu
