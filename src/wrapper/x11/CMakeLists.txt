find_package(X11 REQUIRED COMPONENTS Xrandr)

# Stable connector IDs for saved screen-output monitor selections.
add_library(x11_dependencies INTERFACE)
target_link_libraries(x11_dependencies INTERFACE X11::X11 X11::Xrandr)
