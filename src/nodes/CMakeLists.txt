list(APPEND MIXIMUS_MAIN_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/interface_type.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/interface_type.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/connection.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/connection.cpp
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
add_subdirectory(math)
add_subdirectory(config)

set(MIXIMUS_MAIN_SOURCES ${MIXIMUS_MAIN_SOURCES} PARENT_SCOPE)