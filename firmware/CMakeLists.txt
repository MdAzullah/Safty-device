cmake_minimum_required(VERSION 3.16)

# Set the extra component directory if sources are in 'src' instead of 'main'
set(EXTRA_COMPONENT_DIRS "src")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(firmware)
