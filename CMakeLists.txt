cmake_minimum_required(VERSION 3.5)
set(ProjectName miximus)
project(${ProjectName})

set (CMAKE_CXX_STANDARD 17)

add_compile_definitions(
    ASIO_STANDALONE
    _WEBSOCKETPP_CPP11_INTERNAL_
    ASIO_HAS_STD_TYPE_TRAITS
    ASIO_HAS_STD_SHARED_PTR 
    ASIO_HAS_STD_ADDRESSOF
    ASIO_HAS_STD_ATOMIC
    ASIO_HAS_STD_CHRONO 
    ASIO_HAS_CSTDINT
    ASIO_HAS_STD_ARRAY 
    ASIO_HAS_STD_SYSTEM_ERROR
)

add_subdirectory(submodules)
add_subdirectory(static)
add_subdirectory(src)

set_target_properties(example minigzip zlib  
    PROPERTIES EXCLUDE_FROM_ALL 1 EXCLUDE_FROM_DEFAULT_BUILD 1
    )

set_target_properties(${ProjectName}
    PROPERTIES
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    )

