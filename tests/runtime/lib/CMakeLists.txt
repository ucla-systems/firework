add_library(firework SHARED syncQueue.cpp utils.cpp runtime.cpp)


# other options
# when debugging: -g -O0
target_compile_options(firework PRIVATE -no-pie -O3)