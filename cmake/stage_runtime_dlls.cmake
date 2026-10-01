# Several executables may share a directory and be built concurrently.
file(MAKE_DIRECTORY "${RUNTIME_DIRECTORY}")
file(LOCK "${RUNTIME_DIRECTORY}/.miximus-runtime-dlls.lock" GUARD PROCESS)
foreach(dll IN LISTS RUNTIME_DLLS)
    cmake_path(GET dll FILENAME name)
    file(COPY_FILE "${dll}" "${RUNTIME_DIRECTORY}/${name}" ONLY_IF_DIFFERENT)
endforeach()
