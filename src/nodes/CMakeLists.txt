list(APPEND MIXIMUS_MAIN_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/node.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/interface.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/interface.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/manager.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/manager.cpp
)

add_subdirectory(dummy)
add_subdirectory(decklink)
add_subdirectory(math)
add_subdirectory(adapters)

set(MIXIMUS_MAIN_SOURCES ${MIXIMUS_MAIN_SOURCES} PARENT_SCOPE)