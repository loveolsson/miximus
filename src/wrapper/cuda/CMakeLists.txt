find_package(CUDAToolkit REQUIRED)

add_library(cuda INTERFACE)
target_link_libraries(cuda INTERFACE CUDA::cudart)
