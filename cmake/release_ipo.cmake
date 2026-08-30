include(CheckIPOSupported)

option(MIXIMUS_ENABLE_RELEASE_IPO "Enable interprocedural optimization in Release builds" OFF)

if(NOT MIXIMUS_ENABLE_RELEASE_IPO)
    return()
endif()

block()
    check_ipo_supported(
        RESULT MIXIMUS_RELEASE_IPO_SUPPORTED
        OUTPUT MIXIMUS_RELEASE_IPO_ERROR
        LANGUAGES CXX
    )

    if(MIXIMUS_RELEASE_IPO_SUPPORTED)
        set(MIXIMUS_RELEASE_IPO_TARGETS
            miximus
            app
            gpu
            nodes
            render
            web_server
            logger
            media
            miximus_types
            shutdown_watchdog
        )

        foreach(OPTIONAL_TARGET core_test render_test miximus_typescript_generator)
            if(TARGET ${OPTIONAL_TARGET})
                list(APPEND MIXIMUS_RELEASE_IPO_TARGETS ${OPTIONAL_TARGET})
            endif()
        endforeach()

        foreach(TARGET_NAME IN LISTS MIXIMUS_RELEASE_IPO_TARGETS)
            set_property(TARGET ${TARGET_NAME} PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
        endforeach()
    else()
        message(WARNING "Release IPO is not supported: ${MIXIMUS_RELEASE_IPO_ERROR}")
    endif()
endblock()
