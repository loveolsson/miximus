# Use the exact source and patch series of the working Linux distribution package.
vcpkg_download_distfile(SOURCE_ARCHIVE
    URLS "https://archive.ubuntu.com/ubuntu/pool/universe/w/websocketpp/websocketpp_0.8.2+git20250909.orig.tar.gz"
    FILENAME "websocketpp_0.8.2+git20250909.orig.tar.gz"
    SHA512 e98d89189579ad2fe91806c6fe1361e1c38799e9b77711a220bb32d013763880ed1681a3cef8f276d7f497bd96244308daf22ccd096127ab73f80b9a70012cf6
)
vcpkg_download_distfile(DEBIAN_ARCHIVE
    URLS "https://archive.ubuntu.com/ubuntu/pool/universe/w/websocketpp/websocketpp_0.8.2+git20250909-2.debian.tar.xz"
    FILENAME "websocketpp_0.8.2+git20250909-2.debian.tar.xz"
    SHA512 aa5c3f3f9f059bd7df5a90dea3e638b1048e47cba5ee9800aa3959ef505659af1a359293a5cb17cad4f33c7f9c5a2bd4c0587614fac139dd33bac2b614f2cd46
)
vcpkg_extract_source_archive(DEBIAN_PATH ARCHIVE "${DEBIAN_ARCHIVE}" NO_REMOVE_ONE_LEVEL)
file(STRINGS "${DEBIAN_PATH}/debian/patches/series" PATCH_SERIES)
if(NOT PATCH_SERIES STREQUAL "1190.patch")
    message(FATAL_ERROR "Unexpected WebSocket++ distribution patch series: ${PATCH_SERIES}")
endif()
vcpkg_extract_source_archive(SOURCE_PATH
    ARCHIVE "${SOURCE_ARCHIVE}"
    PATCHES "${DEBIAN_PATH}/debian/patches/1190.patch"
)
file(COPY "${SOURCE_PATH}/websocketpp" DESTINATION "${CURRENT_PACKAGES_DIR}/include"
    FILES_MATCHING PATTERN "*.hpp")
file(MAKE_DIRECTORY "${CURRENT_PACKAGES_DIR}/share/${PORT}")
set(PACKAGE_INSTALL_INCLUDE_DIR "\${CMAKE_CURRENT_LIST_DIR}/../../include")
set(WEBSOCKETPP_VERSION 0.8.3)
set(PACKAGE_INIT "macro(set_and_check)\n  set(\${ARGV})\nendmacro()")
configure_file("${SOURCE_PATH}/websocketpp-config.cmake.in"
    "${CURRENT_PACKAGES_DIR}/share/${PORT}/websocketpp-config.cmake" @ONLY)
configure_file("${SOURCE_PATH}/COPYING" "${CURRENT_PACKAGES_DIR}/share/${PORT}/copyright" COPYONLY)
configure_file("${DEBIAN_PATH}/debian/copyright"
    "${CURRENT_PACKAGES_DIR}/share/${PORT}/debian-copyright" COPYONLY)
