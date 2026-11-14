cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pimoroni_pga2350 CACHE STRING "Board type")
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(n64link C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()
include(${CMAKE_CURRENT_LIST_DIR}/../common/dccart_common.cmake)

add_executable(n64link src/main.c src/commands.c src/n64_bus.c src/cic.c src/cart_info.c src/cart_diag.c
    src/cart_flashram.c src/cart_eeprom.c src/cart_pass.c src/cart_power.c src/input/onewire.c
    ${DCCART_COMMON}/cart_sram.c)
target_include_directories(n64link PRIVATE src)
target_compile_definitions(n64link PRIVATE CART_SAVE_MAX=2048u CART_SAVE_BLOCK=8u)  # the joybus EEPROM
pico_generate_pio_header(n64link ${CMAKE_CURRENT_LIST_DIR}/src/n64_bus.pio)
pico_generate_pio_header(n64link ${CMAKE_CURRENT_LIST_DIR}/src/cic_read.pio)
pico_generate_pio_header(n64link ${CMAKE_CURRENT_LIST_DIR}/src/input/onewire.pio)
target_link_libraries(n64link hardware_gpio hardware_pwm hardware_flash)
dccart_firmware(n64link)
