cmake_minimum_required(VERSION 3.20)
project(fgvk LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BG3SE "C:/Dev/bg3se-upscaler/External")
add_library(fgvk SHARED
  src/dllmain.cpp src/log.cpp src/vkhooks.cpp src/slboot.cpp src/inputs.cpp)
target_include_directories(fgvk PRIVATE
  "${BG3SE}/VulkanSDK/Include" "${BG3SE}/streamline/include" "${BG3SE}/Detours/include" src)
target_link_libraries(fgvk PRIVATE "${BG3SE}/Detours/lib.X64/detours.lib")
target_compile_definitions(fgvk PRIVATE VK_NO_PROTOTYPES WIN32_LEAN_AND_MEAN NOMINMAX)
set_target_properties(fgvk PROPERTIES OUTPUT_NAME "fgvk")
