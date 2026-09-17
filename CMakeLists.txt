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
  src/camera_session.cpp
  src/camera_session.hpp
  src/camera_thread.cpp
  src/camera_thread.hpp
  src/fixed_pattern_correction.cpp
  src/fixed_pattern_correction.hpp
  src/fixed_pattern_profile_store.cpp
  src/fixed_pattern_profile_store.hpp
  src/main_window.cpp
  src/main_window.hpp
  src/thermal_image_widget.cpp
  src/thermal_image_widget.hpp
  src/thermal_inspection.cpp
  src/thermal_inspection.hpp
  src/radiometric_correction.cpp
  src/radiometric_correction.hpp
  src/seek_compact_usb.cpp
  src/seek_compact_usb.hpp
  src/thermal_palette.cpp
  src/thermal_palette.hpp
  src/thermal_renderer.cpp
  src/thermal_renderer.hpp
  src/thermal_processor.cpp
  src/thermal_processor.hpp
  src/thermal_frame.hpp
  src/tr256i_camera.cpp
  src/tr256i_camera.hpp
  src/tr256i_decoder.cpp
  src/tr256i_decoder.hpp
)
target_link_libraries(thermalseek PRIVATE Qt6::Widgets PkgConfig::LIBUSB)
target_compile_options(
  thermalseek
  PRIVATE
    $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
)

if(BUILD_TESTING)
  add_executable(
    tr256i_decoder_test
    tests/tr256i_decoder_test.cpp
    src/tr256i_decoder.cpp
  )
  target_include_directories(
    tr256i_decoder_test
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_options(
    tr256i_decoder_test
    PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
  )
  add_test(NAME tr256i_decoder_test COMMAND tr256i_decoder_test)

  add_executable(
    radiometric_correction_test
    tests/radiometric_correction_test.cpp
    src/radiometric_correction.cpp
  )
  target_include_directories(
    radiometric_correction_test
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_compile_options(
    radiometric_correction_test
    PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
  )
  add_test(NAME radiometric_correction_test COMMAND radiometric_correction_test)

  add_executable(
    fixed_pattern_correction_test
    tests/fixed_pattern_correction_test.cpp
    src/fixed_pattern_correction.cpp
    src/fixed_pattern_profile_store.cpp
  )
  target_include_directories(
    fixed_pattern_correction_test
    PRIVATE
      ${CMAKE_CURRENT_SOURCE_DIR}/src
  )
  target_link_libraries(fixed_pattern_correction_test PRIVATE Qt6::Core)
  target_compile_options(
    fixed_pattern_correction_test
    PRIVATE
      $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>
  )
  add_test(
    NAME fixed_pattern_correction_test
    COMMAND fixed_pattern_correction_test
  )

  add_executable(
    thermal_renderer_test
    tests/thermal_renderer_test.cpp
    src/thermal_palette.cpp
    src/radiometric_correction.cpp
    src/thermal_renderer.cpp
    src/thermal_inspection.cpp
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
