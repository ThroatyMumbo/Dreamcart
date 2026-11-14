# Shared by every cart reader: include() after pico_sdk_init(), then dccart_firmware(<target>).
set(DCCART_COMMON ${CMAKE_CURRENT_LIST_DIR})

add_library(dccart_common INTERFACE)
target_sources(dccart_common INTERFACE
    ${DCCART_COMMON}/boot_reset.S
    ${DCCART_COMMON}/boot.c
    ${DCCART_COMMON}/fault.c
    ${DCCART_COMMON}/console.c
    ${DCCART_COMMON}/maple_cmds.c
    ${DCCART_COMMON}/cart_stream.c
    ${DCCART_COMMON}/crc_dma.c
    ${DCCART_COMMON}/pio_share.c
    ${DCCART_COMMON}/psram.c
    ${DCCART_COMMON}/sd.c
    ${DCCART_COMMON}/sd_spi.c
    ${DCCART_COMMON}/sd_sdio.c
    ${DCCART_COMMON}/sd_cmds.c
    ${DCCART_COMMON}/crc32.c
    ${DCCART_COMMON}/xcache.c
    ${DCCART_COMMON}/maple/mapledev.c
    ${DCCART_COMMON}/maple/maple_proto.c
    ${DCCART_COMMON}/maple/maple_cart.c
    ${DCCART_COMMON}/maple/dc_link.c)
target_include_directories(dccart_common INTERFACE ${DCCART_COMMON} ${DCCART_COMMON}/maple)
pico_generate_pio_header(dccart_common ${DCCART_COMMON}/maple/mapledev.pio)
pico_generate_pio_header(dccart_common ${DCCART_COMMON}/sd_sdio.pio)
target_link_libraries(dccart_common INTERFACE pico_stdlib pico_multicore
    hardware_pio hardware_dma hardware_clocks hardware_vreg hardware_watchdog hardware_spi hardware_xip_cache)

# UART1 on GP40/GP41: the PGA2350 default of UART0 on GP0/GP1 collides with every cart bus
target_compile_definitions(dccart_common INTERFACE
    PICO_CRT0_NO_RESET_SECTION=1
    PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK=1  # clk_peri at 200 MHz, not 48: SPI1 reaches 50 MHz
    PICO_DEFAULT_UART=1
    PICO_DEFAULT_UART_TX_PIN=40
    PICO_DEFAULT_UART_RX_PIN=41)

function(dccart_firmware target)
    target_link_libraries(${target} dccart_common)
    # core1's Maple loop must never wait on XIP behind core0; boot_reset.S also requires it
    pico_set_binary_type(${target} copy_to_ram)
    pico_enable_stdio_uart(${target} 1)
    pico_add_extra_outputs(${target})
endfunction()
