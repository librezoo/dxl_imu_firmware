cmake_minimum_required(VERSION 3.16)
project(imu_power_core C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(imu_power_core
    src/config.c
    src/device.c
    src/dxl.c
    src/imu_wire.c
    src/maintenance.c
)
target_include_directories(imu_power_core PUBLIC include)

add_executable(imu_power_core_tests tests/test_core.c)
target_link_libraries(imu_power_core_tests PRIVATE imu_power_core)
enable_testing()
add_test(NAME imu_power_core_tests COMMAND imu_power_core_tests)

# The MCU SDK is committed with the firmware; no external SDK is required.
option(IMU_POWER_BUILD_CH32 "Build CH32V203C8U6 firmware" OFF)
if(IMU_POWER_BUILD_CH32)
    enable_language(ASM)
    set(IMU_POWER_WCH_EVT "${CMAKE_CURRENT_SOURCE_DIR}/vendor/wch-ch32v20x/EVT")
    if(NOT EXISTS "${IMU_POWER_WCH_EVT}/EXAM/SRC")
        message(FATAL_ERROR "The committed vendor/wch-ch32v20x/EVT SDK is missing")
    endif()
    set(WCH_SRC "${IMU_POWER_WCH_EVT}/EXAM/SRC")
    set(WCH_PERIPH "${WCH_SRC}/Peripheral")
    set(WCH_SYSTEM_DIR "${IMU_POWER_WCH_EVT}/EXAM/USART/USART_DMA/User")
    # Reserve the final two 4 KiB pages for the persistent configuration. The
    # rest of the linker script remains the official WCH D6 script.
    file(READ "${WCH_SRC}/Ld/Link.ld" WCH_LINK_TEXT)
    string(REGEX MATCHALL "LENGTH = 64K" WCH_FLASH_REGIONS "${WCH_LINK_TEXT}")
    list(LENGTH WCH_FLASH_REGIONS WCH_FLASH_REGION_COUNT)
    if(NOT WCH_FLASH_REGION_COUNT EQUAL 1)
        message(FATAL_ERROR "Expected exactly one 64K flash region in the WCH linker script")
    endif()
    string(REPLACE "LENGTH = 64K" "LENGTH = 56K" IMU_LINK_TEXT "${WCH_LINK_TEXT}")
    file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/imu_power_link.ld"
         CONTENT "${IMU_LINK_TEXT}")
    add_executable(imu_power_ch32
        ${WCH_SRC}/Core/core_riscv.c
        ${WCH_PERIPH}/src/ch32v20x_gpio.c
        ${WCH_PERIPH}/src/ch32v20x_rcc.c
        ${WCH_PERIPH}/src/ch32v20x_spi.c
        ${WCH_PERIPH}/src/ch32v20x_usart.c
        ${WCH_PERIPH}/src/ch32v20x_dma.c
        ${WCH_PERIPH}/src/ch32v20x_exti.c
        ${WCH_PERIPH}/src/ch32v20x_flash.c
        ${WCH_PERIPH}/src/ch32v20x_iwdg.c
        ${WCH_PERIPH}/src/ch32v20x_misc.c
        ${WCH_SYSTEM_DIR}/system_ch32v20x.c
        platform/ch32_main.c
        platform/ch32v203_port.c
        ${WCH_SRC}/Startup/startup_ch32v20x_D6.S
        src/config.c src/device.c src/dxl.c src/imu_wire.c src/maintenance.c
    )
    target_compile_definitions(imu_power_ch32 PRIVATE IMU_POWER_CH32 CH32V20x_D6)
    target_include_directories(imu_power_ch32 PRIVATE
        include platform ${WCH_SRC}/Core ${WCH_SRC}/Debug
        ${WCH_PERIPH}/inc ${WCH_SYSTEM_DIR})
    target_compile_options(imu_power_ch32 PRIVATE -Os -march=rv32imac -mabi=ilp32
        -msmall-data-limit=8 -mno-save-restore -ffunction-sections -fdata-sections)
    target_link_options(imu_power_ch32 PRIVATE -march=rv32imac -mabi=ilp32
        --specs=nano.specs --specs=nosys.specs
        -T${CMAKE_CURRENT_BINARY_DIR}/imu_power_link.ld -Wl,--gc-sections -nostartfiles
        -Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/imu_power_ch32.map)
    add_custom_command(TARGET imu_power_ch32 POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O ihex
            $<TARGET_FILE:imu_power_ch32> ${CMAKE_CURRENT_BINARY_DIR}/imu_power_ch32.hex
        COMMAND ${CMAKE_OBJCOPY} -O binary
            $<TARGET_FILE:imu_power_ch32> ${CMAKE_CURRENT_BINARY_DIR}/imu_power_ch32.bin
        COMMENT "Generating CH32 HEX and BIN images"
        VERBATIM)
endif()
