set(DECKLINK_SDK_DIR ${CMAKE_SOURCE_DIR}/3rd-party/decklink-sdk)

if(WIN32) 
    set(DECKLINK_OS_DIR ${DECKLINK_SDK_DIR}/Win)
    find_file(DECKLINK_SOURCES DeckLinkAPI.idl PATHS ${DECKLINK_OS_DIR}/include REQUIRED NO_DEFAULT_PATH)
    
    add_library(decklink
        ${DECKLINK_SOURCES}
    )
else()
    if(APPLE)
        set(DECKLINK_OS_DIR ${DECKLINK_SDK_DIR}/Mac)
    elseif(UNIX AND NOT APPLE)
        set(DECKLINK_OS_DIR ${DECKLINK_SDK_DIR}/Linux)
    endif()

    find_file(DECKLINK_SOURCES DeckLinkAPIDispatch.cpp PATHS ${DECKLINK_OS_DIR}/include REQUIRED NO_DEFAULT_PATH)
    find_file(DECKLINK_DVP_LIB libdvp.so.1 PATHS ${DECKLINK_OS_DIR}/Samples/NVIDIA_GPUDirect/x86_64 REQUIRED NO_DEFAULT_PATH)

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