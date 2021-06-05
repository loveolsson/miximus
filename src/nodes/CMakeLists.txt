list(APPEND MIXIMUS_MAIN_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/connection_type.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/connection.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/connection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node_manager.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node_type.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node.cpp
)

add_subdirectory(dummy)

set(MIXIMUS_MAIN_SOURCES ${MIXIMUS_MAIN_SOURCES} PARENT_SCOPE)