if(EXISTS "${WEB_BUILD_FAILED_FILE}")
    file(READ "${WEB_BUILD_FAILED_FILE}" _web_err)
    string(STRIP "${_web_err}" _web_err)
    message(WARNING
        "\n"
        "  WARNING: Building the Web UI failed (${_web_err}).\n"
        "  The bundled version remains the previous successful build.\n"
        "  The full build error can be read further up in the build log.\n"
    )
endif()
