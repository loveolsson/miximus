add_library(messages INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/messages/message.hpp
)

target_include_directories(messages INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_include_directories(messages INTERFACE
    ${CMAKE_SOURCE_DIR}/submodules/json/include
)