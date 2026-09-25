cmake_minimum_required(VERSION 3.10.0)
project(drm-lab VERSION 0.1.0 LANGUAGES C CXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_executable(drm-lab main.cpp    
    src/device.cpp
)

target_include_directories(drm-lab 
    PUBLIC ${PROJECT_SOURCE_DIR}/include)