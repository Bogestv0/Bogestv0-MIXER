cmake_minimum_required(VERSION 3.20)
project(bogestv0_engine C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
  # Статическая среда выполнения: exe запускается без установки Visual C++ Redistributable.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
  add_compile_options(/utf-8 /W3)
endif()

# Шумоподавление RNNoise (BSD-3, см. third_party/rnnoise/COPYING). Веса нейросети лежат в rnn_data.c.
set(RNN ${CMAKE_CURRENT_SOURCE_DIR}/third_party/rnnoise)
add_library(rnnoise STATIC
  ${RNN}/src/denoise.c ${RNN}/src/rnn.c ${RNN}/src/rnn_data.c ${RNN}/src/pitch.c
  ${RNN}/src/kiss_fft.c ${RNN}/src/celt_lpc.c)
target_include_directories(rnnoise PUBLIC ${RNN}/include PRIVATE ${RNN}/src)
if(MSVC)
  # Чужой код: предупреждения про преобразование типов и устаревшие функции не нужны.
  target_compile_options(rnnoise PRIVATE /W1 /wd4244 /wd4267 /wd4305 /wd4996)
  target_compile_definitions(rnnoise PRIVATE _CRT_SECURE_NO_WARNINGS _USE_MATH_DEFINES)
else()
  target_compile_options(rnnoise PRIVATE -w -O2)
endif()

add_executable(engine main.cpp)
target_link_libraries(engine PRIVATE rnnoise)
if(WIN32)
  # Без этого windows.h подменяет std::min / std::max своими макросами.
  target_compile_definitions(engine PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)
endif()
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(engine PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
