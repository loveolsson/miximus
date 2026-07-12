set(DVP_SDK_DIR ${CMAKE_SOURCE_DIR}/3rd-party)

add_library(dvp INTERFACE)

if(WIN32)
    find_file(DVP_LIB dvp_static.lib
        PATHS ${DVP_SDK_DIR}/dvp170_win/lib/x64
        REQUIRED NO_DEFAULT_PATH)
    target_include_directories(dvp SYSTEM INTERFACE
        ${DVP_SDK_DIR}/dvp170_win/include)
    target_link_libraries(dvp INTERFACE ${DVP_LIB})

elseif(UNIX AND NOT APPLE)
    # Use the static library — no .so runtime dependency, no version mismatch risk.
    find_file(DVP_LIB libdvp.a
        PATHS ${DVP_SDK_DIR}/dvp170_linux/x64
        REQUIRED NO_DEFAULT_PATH)
    target_include_directories(dvp SYSTEM INTERFACE
        ${DVP_SDK_DIR}/dvp170_linux/x64)
    target_link_libraries(dvp INTERFACE ${DVP_LIB} X11 Xext XNVCtrl)
endif()
