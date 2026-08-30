add_subdirectory(magic_enum)

set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
set(SPDLOG_SYSTEM_INCLUDES ON CACHE BOOL "" FORCE)
set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "" FORCE)
add_subdirectory(spdlog EXCLUDE_FROM_ALL)

# Suppress deprecation warnings from external dependencies
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
add_subdirectory(glad/cmake)
set(CMAKE_WARN_DEPRECATED ON CACHE BOOL "" FORCE)

glad_add_library(glad_gl REPRODUCIBLE LANGUAGE C API gl:core=4.6)
target_link_libraries(glad_gl PRIVATE ${CMAKE_DL_LIBS})
