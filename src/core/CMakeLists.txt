list(APPEND MIXIMUS_MAIN_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/app_state.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/app_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node_manager.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/node_manager.cpp
)

add_subdirectory(adapters)

set(MIXIMUS_MAIN_SOURCES ${MIXIMUS_MAIN_SOURCES} PARENT_SCOPE)