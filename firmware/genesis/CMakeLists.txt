cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pimoroni_pga2350 CACHE STRING "Board type")
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(mdlink C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()
include(${CMAKE_CURRENT_LIST_DIR}/../common/dccart_common.cmake)

add_executable(mdlink src/main.c src/commands.c src/md_power.c src/md_bus.c src/md_map.c src/md_cart.c
    src/cart_diag.c ${DCCART_COMMON}/cart_sram.c ${DCCART_COMMON}/bus595.c)
target_include_directories(mdlink PRIVATE src)
pico_generate_pio_header(mdlink ${CMAKE_CURRENT_LIST_DIR}/src/md_bus.pio)
target_link_libraries(mdlink hardware_gpio hardware_clocks hardware_dma)
dccart_firmware(mdlink)
