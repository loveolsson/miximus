#include "context_logging.hpp"
#include "logger/logger.hpp"

#include <string_view>

namespace {
using namespace miximus;

const auto _log = [] { return getlog("gpu"); };

/**
 * @brief Get string representation of OpenGL enum values for debugging
 * @param v OpenGL enum value (source, type, severity, etc.)
 * @return Human-readable string representation
 */
constexpr std::string_view get_opengl_string_from_enum(GLenum v)
{
    switch (v) {
        // Debug sources
        case GL_DEBUG_SOURCE_API:
            return "API";
        case GL_DEBUG_SOURCE_OTHER:
            return "Other";

        // Debug types
        case GL_DEBUG_TYPE_ERROR:
            return "Error";
        case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR:
            return "Deprecated Behaviour";
        case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR:
            return "Undefined Behaviour";
        case GL_DEBUG_TYPE_PORTABILITY:
            return "Portability";
        case GL_DEBUG_TYPE_PERFORMANCE:
            return "Performance";
        case GL_DEBUG_TYPE_MARKER:
            return "Marker";
        case GL_DEBUG_TYPE_PUSH_GROUP:
            return "Push Group";
        case GL_DEBUG_TYPE_POP_GROUP:
            return "Pop Group";
        case GL_DEBUG_TYPE_OTHER:
            return "Other";

        // Debug severities
        case GL_DEBUG_SEVERITY_HIGH:
            return "High";
        case GL_DEBUG_SEVERITY_MEDIUM:
            return "Medium";
        case GL_DEBUG_SEVERITY_LOW:
            return "Low";
        case GL_DEBUG_SEVERITY_NOTIFICATION:
            return "Notification";

        default:
            return "UNKNOWN";
    }
}

} // namespace

namespace miximus::gpu {

void GLAPIENTRY opengl_error_callback(GLenum        source,
                                      GLenum        type,
                                      GLuint        id,
                                      GLenum        severity,
                                      GLsizei       length,
                                      const GLchar* message,
                                      const void*   userParam)
{
    (void)id;
    (void)userParam;

    if (severity == GL_DEBUG_SEVERITY_NOTIFICATION) {
        return;
    }

    const auto source_str   = get_opengl_string_from_enum(source);
    const auto type_str     = get_opengl_string_from_enum(type);
    const auto severity_str = get_opengl_string_from_enum(severity);
    const auto message_str  = std::string_view(message, length);

    if (type == GL_DEBUG_TYPE_ERROR) {
        _log()->error("OpenGL error: source = {}, type = '{}', severity = '{}', message = '{}'",
                      source_str,
                      type_str,
                      severity_str,
                      message_str);
    } else {
        _log()->warn("OpenGL warning: source = {}, type = '{}', severity = '{}', message = '{}'",
                     source_str,
                     type_str,
                     severity_str,
                     message_str);
    }
}

void glfw_error_callback(int error, const char* description)
{
    _log()->error("GLFW error: [{}]:{}", error, description);
}

} // namespace miximus::gpu
