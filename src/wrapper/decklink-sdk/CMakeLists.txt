set(DECKLINK_SDK_DIR ${CMAKE_SOURCE_DIR}/3rd-party/decklink-sdk)

if(WIN32) 
    find_package(IDL REQUIRED)

    set(DECKLINK_OS_DIR ${DECKLINK_SDK_DIR}/Win)
    find_file(DECKLINK_SOURCES DeckLinkAPI.idl PATHS ${DECKLINK_OS_DIR}/include REQUIRED NO_DEFAULT_PATH)
    find_file(DECKLINK_DVP_LIB dvp_static.lib PATHS ${DECKLINK_OS_DIR}/Samples/NVIDIA_GPUDirect/lib/x64 REQUIRED NO_DEFAULT_PATH)

    file(RELATIVE_PATH DECKLINK_SOURCES_REL ${CMAKE_CURRENT_SOURCE_DIR} ${DECKLINK_SOURCES})

    add_idl(DeckLinkAPI ${DECKLINK_SOURCES_REL})    
    add_library(decklink ${CMAKE_CURRENT_BINARY_DIR}/Generated/DeckLinkAPI_i.c)

    target_link_libraries(decklink PUBLIC
        DeckLinkAPI
        ${DECKLINK_DVP_LIB}
#        comsuppw
    )
else()
    if(APPLE)
        set(DECKLINK_OS_DIR ${DECKLINK_SDK_DIR}/Mac)
    elseif(UNIX AND NOT APPLE)
        set(DECKLINK_OS_DIR ${DECKLINK_SDK_DIR}/Linux)
    endif()

    find_file(DECKLINK_SOURCES DeckLinkAPIDispatch.cpp PATHS ${DECKLINK_OS_DIR}/include REQUIRED)
    find_file(DECKLINK_DVP_LIB libdvp.so.1 PATHS ${DECKLINK_OS_DIR}/Samples/NVIDIA_GPUDirect/x86_64 REQUIRED)

    add_library(decklink
        ${DECKLINK_SOURCES}
    )

    target_include_directories(decklink PUBLIC 
        ${DECKLINK_OS_DIR}/include
        ${DECKLINK_OS_DIR}/Samples/NVIDIA_GPUDirect/include
    )

    target_link_libraries(decklink PUBLIC
        ${DECKLINK_DVP_LIB}
    )
endif()