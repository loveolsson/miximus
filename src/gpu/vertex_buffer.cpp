#include "vertex_buffer.hpp"

#include "context.hpp"
#include "glad.hpp"

namespace miximus::gpu {

vertex_buffer_s::vertex_buffer_s() { glGenBuffers(1, &id_); }

vertex_buffer_s::~vertex_buffer_s()
{
    if (!context_s::require_current()) {
        return;
    }
    glDeleteBuffers(1, &id_);
}

void vertex_buffer_s::bind() const { glBindBuffer(GL_ARRAY_BUFFER, id_); }

void vertex_buffer_s::set_data_bytes(std::span<const std::byte> data, size_t element_count)
{
    vertex_count_ = element_count;

    glNamedBufferData(id_, static_cast<GLsizeiptr>(data.size_bytes()), data.data(), GL_STATIC_DRAW);
}

void vertex_buffer_s::draw() const { glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(vertex_count_)); }

} // namespace miximus::gpu
