# It is a little bit weird that image is placed here, rather than in render, 
# but since it is currently only used context creation, it's here to avoid 
# circular dependencies

target_sources(gpu
PRIVATE
    stb_impl.cpp
)