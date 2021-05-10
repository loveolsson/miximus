list(APPEND MIXIMUS_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/connection_type.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/connection.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/connection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node_manager.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node_manager.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node_type.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node.cpp
)

set(MIXIMUS_SOURCES ${MIXIMUS_SOURCES} PARENT_SCOPE)