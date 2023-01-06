set(QUIC_BUILD_TOOLS OFF)
set(QUIC_BUILD_TEST OFF)
set(QUIC_BUILD_PERF OFF)
#add_subdirectory(msquic)

add_subdirectory(magic_enum)

add_subdirectory(glad)

target_link_libraries(glad
PRIVATE
    ${CMAKE_DL_LIBS}
)