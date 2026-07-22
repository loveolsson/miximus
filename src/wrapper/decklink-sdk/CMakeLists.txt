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
    file(GLOB DECKLINK_IDL_DEPENDENCIES CONFIGURE_DEPENDS ${DECKLINK_OS_DIR}/include/*.idl)

    set(MIDL_OUTPUT_PATH ${CMAKE_CURRENT_BINARY_DIR}/Generated)
    set(MIDL_HEADER ${MIDL_OUTPUT_PATH}/DeckLinkAPI.h)
    set(MIDL_IID_SOURCE ${MIDL_OUTPUT_PATH}/DeckLinkAPI_i.c)

    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(DECKLINK_MIDL_ENV amd64)
    else()
        set(DECKLINK_MIDL_ENV win32)
    endif()

    add_custom_command(
        OUTPUT
            ${MIDL_HEADER}
            ${MIDL_IID_SOURCE}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${MIDL_OUTPUT_PATH}
        COMMAND midl
            /nologo
            /env ${DECKLINK_MIDL_ENV}
            /I ${DECKLINK_OS_DIR}/include
            /out ${MIDL_OUTPUT_PATH}
            /h DeckLinkAPI.h
            /iid DeckLinkAPI_i.c
            ${MIDL_FILE}
        DEPENDS ${DECKLINK_IDL_DEPENDENCIES}
        VERBATIM
    )

    add_library(decklink STATIC
        ${MIDL_HEADER}
        ${MIDL_IID_SOURCE}
        ${DECKLINK_COMMON_SOURCES}
        ${DECKLINK_PLATFORM_SOURCE}
    )

    target_link_libraries(decklink
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

    add_library(decklink STATIC
        ${DECKLINK_SOURCES}
        ${DECKLINK_COMMON_SOURCES}
        ${DECKLINK_PLATFORM_SOURCE}
    )

    target_include_directories(decklink SYSTEM PUBLIC
        ${DECKLINK_OS_DIR}/include
    )

    target_link_libraries(decklink PRIVATE
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )

    if(APPLE)
        find_library(COREFOUNDATION_FRAMEWORK CoreFoundation REQUIRED)
        target_link_libraries(decklink PRIVATE ${COREFOUNDATION_FRAMEWORK})
    endif()
endif()
