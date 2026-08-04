cmake_minimum_required(VERSION 3.22.1)
project(archstreamer_ds_touch LANGUAGES CXX)

add_library(archstreamer_ds_touch SHARED ds_touch_jni.cpp)

# ArchStreamer/include (six levels up from app/src/main/cpp).
target_include_directories(archstreamer_ds_touch PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../include")

target_compile_features(archstreamer_ds_touch PRIVATE cxx_std_20)
target_compile_options(archstreamer_ds_touch PRIVATE -fno-exceptions -fno-rtti)

find_library(log-lib log)
target_link_libraries(archstreamer_ds_touch PRIVATE ${log-lib})
