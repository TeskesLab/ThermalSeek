cmake_minimum_required(VERSION 3.21)

project(ThermalSeek VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(PkgConfig REQUIRED)
find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

add_executable(
  thermalseek
  src/main.cpp
  src/main_window.cpp
  src/main_window.hpp
  src/thermal_image_widget.cpp
  src/thermal_image_widget.hpp
  src/thermal_inspection.cpp
  src/thermal_inspection.hpp
  src/seek_compact_usb.cpp
  src/seek_compact_usb.hpp
  src/seek_camera_thread.cpp
  src/seek_camera_thread.hpp
  src/thermal_palette.cpp
  src/thermal_palette.hpp
  src/thermal_renderer.cpp
  src/thermal_renderer.hpp
  src/thermal_processor.cpp
  src/thermal_processor.hpp
)
target_link_libraries(thermalseek PRIVATE Qt6::Widgets PkgConfig::LIBUSB)
target_compile_options(
  thermalseek
  PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

if(BUILD_TESTING)
  add_executable(
    thermal_renderer_test
    tests/thermal_renderer_test.cpp
    src/thermal_palette.cpp
    src/thermal_renderer.cpp
  )
  target_include_directories(
    thermal_renderer_test
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(thermal_renderer_test PRIVATE Qt6::Widgets)
  target_compile_options(
    thermal_renderer_test
    PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
  )
  add_test(NAME thermal_renderer_test COMMAND thermal_renderer_test)

  add_executable(
    thermal_inspection_test
    tests/thermal_inspection_test.cpp
    src/thermal_inspection.cpp
  )
  target_include_directories(
    thermal_inspection_test
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(thermal_inspection_test PRIVATE Qt6::Widgets)
  target_compile_options(
    thermal_inspection_test
    PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
  )
  add_test(NAME thermal_inspection_test COMMAND thermal_inspection_test)
endif()
