#pragma once

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <glad/glad.h>

namespace miximus::gpu {

/**
 * @brief OpenGL debug message callback for error reporting
 *
 * Filters out notifications and logs errors/warnings with detailed context.
 * Automatically called by OpenGL when debug output is enabled.
 */
void GLAPIENTRY opengl_error_callback(GLenum        source,
                                      GLenum        type,
                                      GLuint        id,
                                      GLenum        severity,
                                      GLsizei       length,
                                      const GLchar* message,
                                      const void*   userParam);

/**
 * @brief GLFW error callback for framework-level error reporting
 * @param error GLFW error code
 * @param description Human-readable error description
 */
void glfw_error_callback(int error, const char* description);

} // namespace miximus::gpu
