set(DECKLINK_SDK_DIR ${CMAKE_SOURCE_DIR}/3rd-party/decklink-sdk)

set(DECKLINK_COMMON_SOURCES 
    decklink_inc.hpp
    decklink_ptr.hpp
)

if(WIN32) 
    set(DECKLINK_OS_DIR ${DECKLINK_SDK_DIR}/Win)
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
    )

    add_dependencies(decklink
    midl-cmplr
    )

    target_link_libraries(decklink PRIVATE
        glad_gl
    )

    target_include_directories(decklink SYSTEM PUBLIC
        ${MIDL_OUTPUT_PATH}
    )
else()
    if(APPLE)
        set(DECKLINK_OS_DIR ${DECKLINK_SDK_DIR}/Mac)
    elseif(UNIX AND NOT APPLE)
        set(DECKLINK_OS_DIR ${DECKLINK_SDK_DIR}/Linux)
    endif()

    find_file(DECKLINK_SOURCES DeckLinkAPIDispatch.cpp PATHS ${DECKLINK_OS_DIR}/include REQUIRED)
    find_file(DECKLINK_TRANSFER_SRC VideoFrameTransfer.cpp PATHS ${DECKLINK_OS_DIR}/Samples/LoopThroughWithOpenGLCompositing/ REQUIRED)

    add_library(decklink SHARED
        ${DECKLINK_SOURCES}
        ${DECKLINK_COMMON_SOURCES}
    )

    target_include_directories(decklink SYSTEM PUBLIC 
        ${DECKLINK_OS_DIR}/include
    )

    target_link_libraries(decklink PRIVATE
        glad_gl
    )

    # Suppress warnings from the SDK's own sources — third-party code we can't modify
    target_compile_options(decklink PRIVATE -w)
endif()