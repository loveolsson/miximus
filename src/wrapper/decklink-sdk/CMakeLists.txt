set(DECKLINK_SDK_DIR ${CMAKE_SOURCE_DIR}/3rd-party/decklink-sdk)

set(DECKLINK_COMMON_SOURCES 
    decklink_inc.hpp
    decklink_iid.hpp
    decklink_ptr.hpp
    platform_compat.hpp
)

if(WIN32) 
    set(DECKLINK_OS_DIR ${DECKLINK_SDK_DIR}/Win)
    set(DECKLINK_PLATFORM_SOURCE platform_compat_win.cpp)
    find_file(MIDL_FILE DeckLinkAPI.idl PATHS ${DECKLINK_OS_DIR}/include REQUIRED NO_DEFAULT_PATH)

    set(MIDL_OUTPUT_PATH ${CMAKE_CURRENT_BINARY_DIR}/Generated)

    set(MIDL_OUTPUT
        ${MIDL_OUTPUT_PATH}/DeckLinkAPI_i.h
        ${MIDL_OUTPUT_PATH}/DeckLinkAPI_i.c
        )

    add_custom_command(
        OUTPUT ${MIDL_OUTPUT}
        COMMAND midl ARGS /nologo ${MIDL_FILE} /out ${MIDL_OUTPUT_PATH} /h ${MIDL_OUTPUT}
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        DEPENDS ${MIDL_FILE}
        VERBATIM
        )

    add_custom_target(midl-cmplr
        DEPENDS ${MIDL_FILE}
        )

    set_source_files_properties(
        ${MIDL_OUTPUT}
            PROPERTIES
                GENERATED TRUE
        )

    add_library(decklink SHARED
        ${MIDL_OUTPUT}
        ${DECKLINK_COMMON_SOURCES}
        ${DECKLINK_PLATFORM_SOURCE}
    )

    add_dependencies(decklink
    midl-cmplr
    )

    target_link_libraries(decklink
    PRIVATE
        glad_gl
    PUBLIC
        Ole32
        OleAut32
    )

    target_include_directories(decklink SYSTEM PUBLIC
        ${MIDL_OUTPUT_PATH}
        ${DECKLINK_OS_DIR}/include
    )
else()
    if(APPLE)
        set(DECKLINK_OS_DIR ${DECKLINK_SDK_DIR}/Mac)
        set(DECKLINK_PLATFORM_SOURCE platform_compat_mac.cpp)
    elseif(UNIX AND NOT APPLE)
        set(DECKLINK_OS_DIR ${DECKLINK_SDK_DIR}/Linux)
        set(DECKLINK_PLATFORM_SOURCE platform_compat_linux.cpp)
    endif()

    find_file(DECKLINK_SOURCES DeckLinkAPIDispatch.cpp PATHS ${DECKLINK_OS_DIR}/include REQUIRED)
    set_source_files_properties(${DECKLINK_SOURCES} PROPERTIES COMPILE_OPTIONS -w)

    add_library(decklink SHARED
        ${DECKLINK_SOURCES}
        ${DECKLINK_COMMON_SOURCES}
        ${DECKLINK_PLATFORM_SOURCE}
    )

    target_include_directories(decklink SYSTEM PUBLIC 
        ${DECKLINK_OS_DIR}/include
    )

    target_link_libraries(decklink PRIVATE
        glad_gl
    )

    if(APPLE)
        find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
        target_link_libraries(decklink PRIVATE ${COREFOUNDATION_FRAMEWORK})
    endif()
endif()

target_compile_definitions(decklink PRIVATE MIXIMUS_DECKLINK_WRAPPER_BUILD)
