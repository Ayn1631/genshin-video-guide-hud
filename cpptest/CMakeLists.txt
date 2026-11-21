cmake_minimum_required(VERSION 3.21)
project(bgi_cpp_locator LANGUAGES CXX)

find_package(OpenCV 4.10 REQUIRED COMPONENTS core imgproc imgcodecs features2d flann calib3d)
find_package(Threads REQUIRED)

add_library(locator_core STATIC map_locator.cpp image.cpp sift.cpp template.cpp index_cache.cpp)
target_compile_features(locator_core PUBLIC cxx_std_17)
target_link_libraries(locator_core PUBLIC ${OpenCV_LIBS} Threads::Threads)
target_include_directories(locator_core PUBLIC ${OpenCV_INCLUDE_DIRS})
set_target_properties(locator_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
if(WIN32)
    target_link_libraries(locator_core PRIVATE bcrypt)
endif()

add_library(bgi_locator SHARED backend.cpp)
target_link_libraries(bgi_locator PRIVATE locator_core)
add_executable(map_locate main.cpp)
target_link_libraries(map_locate PRIVATE locator_core)
add_dependencies(map_locate bgi_locator)

foreach(target locator_core bgi_locator map_locate)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

if(WIN32 AND TARGET opencv_world)
    add_custom_command(TARGET bgi_locator POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:opencv_world>" "$<TARGET_FILE_DIR:bgi_locator>")
endif()

if(WIN32 AND MSVC)
    add_library(bgi_window_capture SHARED window_capture.cpp)
    target_compile_features(bgi_window_capture PRIVATE cxx_std_17)
    target_compile_definitions(bgi_window_capture PRIVATE NOMINMAX)
    target_compile_options(bgi_window_capture PRIVATE /W4 /utf-8 /permissive-)
    target_link_libraries(bgi_window_capture PRIVATE d3d11 windowsapp)
endif()
