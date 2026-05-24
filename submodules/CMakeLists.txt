add_subdirectory(magic_enum)

# Suppress deprecation warnings from external dependencies
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
add_subdirectory(glad/cmake)
set(CMAKE_WARN_DEPRECATED ON CACHE BOOL "" FORCE)

glad_add_library(glad_gl REPRODUCIBLE LANGUAGE C API gl:core=4.6)
target_link_libraries(glad_gl PRIVATE ${CMAKE_DL_LIBS})