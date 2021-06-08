list(APPEND MIXIMUS_MAIN_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/interface_type.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/interface_type.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/connection.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/connection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node_config.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node_config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node_manager.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node_type.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/option.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/option.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/interface.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/interface.cpp
)

add_subdirectory(dummy)
add_subdirectory(decklink)

set(MIXIMUS_MAIN_SOURCES ${MIXIMUS_MAIN_SOURCES} PARENT_SCOPE)