cmake_minimum_required(VERSION 3.16)
project(rtsp_encoder VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Find required packages
find_package(PkgConfig REQUIRED)

pkg_check_modules(GSTREAMER REQUIRED
    gstreamer-1.0
    gstreamer-app-1.0
    gstreamer-video-1.0
    gstreamer-rtp-1.0
    gstreamer-rtsp-server-1.0
)

pkg_check_modules(YAMLCPP REQUIRED yaml-cpp)

# Source files
set(SOURCES
    src/main.cpp
    src/config.cpp
    src/pipeline.cpp
    src/encoder.cpp
    src/stats.cpp
)

# Executable
add_executable(${PROJECT_NAME} ${SOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    ${GSTREAMER_INCLUDE_DIRS}
    ${YAMLCPP_INCLUDE_DIRS}
    src/
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    ${GSTREAMER_LIBRARIES}
    ${YAMLCPP_LIBRARIES}
    pthread
)

target_compile_options(${PROJECT_NAME} PRIVATE
    ${GSTREAMER_CFLAGS_OTHER}
    -Wall -Wextra -O2
)

# Install
install(TARGETS ${PROJECT_NAME} DESTINATION bin)
install(FILES config.yaml DESTINATION etc/rtsp_encoder)
