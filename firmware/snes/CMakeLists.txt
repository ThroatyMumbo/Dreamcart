cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pimoroni_pga2350 CACHE STRING "Board type")
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(sneslink C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()
include(${CMAKE_CURRENT_LIST_DIR}/../common/dccart_common.cmake)

add_executable(sneslink src/main.c src/commands.c src/snes_power.c src/snes_bus.c src/snes_map.c src/snes_cart.c
    src/cart_diag.c ${DCCART_COMMON}/cart_sram.c ${DCCART_COMMON}/bus595.c)
target_include_directories(sneslink PRIVATE src)
pico_generate_pio_header(sneslink ${CMAKE_CURRENT_LIST_DIR}/src/snes_bus.pio)
target_link_libraries(sneslink hardware_gpio hardware_clocks)
dccart_firmware(sneslink)
