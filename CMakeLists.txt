cmake_minimum_required(VERSION 3.10)
project(fideo)

# Set the C standard
set(CMAKE_C_STANDARD 99)

# Find FFmpeg libraries
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET 
    libavcodec 
    libavformat
    libswscale
    libavutil
    libswresample
)
pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)

# Add your executable
add_executable(${CMAKE_PROJECT_NAME} main.c)

target_link_libraries(${CMAKE_PROJECT_NAME} m PkgConfig::FFMPEG PkgConfig::SDL2)
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
