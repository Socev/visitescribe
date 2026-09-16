cmake_minimum_required(VERSION 3.16)

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components/micro-opus")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(visitescribe_opus_modern_benchmark)
