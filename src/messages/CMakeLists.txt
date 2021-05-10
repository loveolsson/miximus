add_library(messages INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/messages/templates.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/messages/transform.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/messages/types.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/include/messages/payload.hpp
)

target_include_directories(messages INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(messages INTERFACE
    ${CMAKE_SOURCE_DIR}/submodules/json/include
    ${CMAKE_SOURCE_DIR}/submodules/eternal/include
)