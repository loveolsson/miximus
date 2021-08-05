set(DECKLINK_SDK_DIR ${CMAKE_SOURCE_DIR}/3rd-party/decklink-sdk)

set(DECKLINK_COMMON_SOURCES 
    decklink_inc.hpp
    decklink_ptr.hpp
)

if(WIN32) 
    set(DECKLINK_OS_DIR ${DECKLINK_SDK_DIR}/Win)
    find_file(MIDL_FILE DeckLinkAPI.idl PATHS ${DECKLINK_OS_DIR}/include REQUIRED NO_DEFAULT_PATH)
    find_file(DECKLINK_DVP_LIB dvp_static.lib PATHS ${DECKLINK_OS_DIR}/Samples/NVIDIA_GPUDirect/lib/x64 REQUIRED NO_DEFAULT_PATH)

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

    add_library(decklink 
        ${MIDL_OUTPUT}
        ${DECKLINK_COMMON_SOURCES}
    )

    add_dependencies(decklink
    midl-cmplr
    )

    target_link_libraries(decklink PRIVATE
        ${DECKLINK_DVP_LIB}
        glad    
    )

    target_include_directories(decklink PUBLIC
        ${MIDL_OUTPUT_PATH}
    )
else()
    if(APPLE)
        set(DECKLINK_OS_DIR ${DECKLINK_SDK_DIR}/Mac)
    elseif(UNIX AND NOT APPLE)
        set(DECKLINK_OS_DIR ${DECKLINK_SDK_DIR}/Linux)
    endif()

    find_file(DECKLINK_SOURCES DeckLinkAPIDispatch.cpp PATHS ${DECKLINK_OS_DIR}/include REQUIRED)
    find_file(DECKLINK_DVP_LIB libdvp.so.1 PATHS ${DECKLINK_OS_DIR}/Samples/NVIDIA_GPUDirect/x86_64 REQUIRED)
    find_file(DECKLINK_TRANSFER_SRC VideoFrameTransfer.cpp PATHS ${DECKLINK_OS_DIR}/Samples/LoopThroughWithOpenGLCompositing/ REQUIRED)

    add_library(decklink
        ${DECKLINK_SOURCES}
        ${DECKLINK_COMMON_SOURCES}
    )

    target_include_directories(decklink PUBLIC 
        ${DECKLINK_OS_DIR}/include
        ${DECKLINK_OS_DIR}/Samples/NVIDIA_GPUDirect/include
    )

    target_link_libraries(decklink PRIVATE
        glad
    )

    target_link_libraries(decklink INTERFACE
        ${DECKLINK_DVP_LIB}
    )
endif()