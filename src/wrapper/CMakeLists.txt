#Unset clang-tidy variables to exempt libraries from checking
set(CMAKE_CXX_CLANG_TIDY "")

#add_subdirectory(cef)
add_subdirectory(decklink-sdk)
add_subdirectory(ffmpeg)
add_subdirectory(video-sdk)
add_subdirectory(ndi-sdk)
add_subdirectory(stb)