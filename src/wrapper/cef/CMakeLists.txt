set(CEF_ROOT ${CMAKE_SOURCE_DIR}/3rd-party/cef)
set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CEF_ROOT}/cmake)

find_package(CEF REQUIRED)
ADD_LOGICAL_TARGET("libcef_lib" "${CEF_LIB_DEBUG}" "${CEF_LIB_RELEASE}")

add_subdirectory(${CEF_LIBCEF_DLL_WRAPPER_PATH} libcef_dll_wrapper)
if(MSVC)
    target_compile_options(libcef_dll_wrapper PRIVATE
        $<$<CONFIG:>:/MD>
        $<$<CONFIG:Debug>:/MDd>
        $<$<CONFIG:Release>:/MD>
    )
endif()

add_library(cef INTERFACE)
target_link_libraries(cef INTERFACE
    libcef_lib
    libcef_dll_wrapper
)
