target_sources(gpu 
PRIVATE
    detail/frame_staging.hpp
    detail/frame_staging.cpp
    detail/transfer_backend.hpp
    detail/transfer_backend.cpp
    detail/transfer_layout.hpp
    detail/transfer_layout.cpp
    detail/transfer_worker.hpp
    texture_transfer.hpp
    texture_upload_fwd.hpp
    texture_upload.hpp
    texture_upload.cpp
    texture_readback_fwd.hpp
    texture_readback.hpp
    texture_readback.cpp
)

if(TARGET cuda_dependencies)
    target_sources(gpu PRIVATE detail/cuda_staging.cpp)
    target_link_libraries(gpu PRIVATE cuda_dependencies)
    target_compile_definitions(gpu PRIVATE MIXIMUS_HAS_CUDA=1)
endif()
