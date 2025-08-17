set(QUIC_BUILD_TOOLS OFF)
set(QUIC_BUILD_TEST OFF)
set(QUIC_BUILD_PERF OFF)
#add_subdirectory(msquic)

add_subdirectory(magic_enum)

# Suppress deprecation warnings from external dependencies
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)
add_subdirectory(glad)
set(CMAKE_WARN_DEPRECATED ON CACHE BOOL "" FORCE)

target_link_libraries(glad
PRIVATE
    ${CMAKE_DL_LIBS}
)