/* Bootloader of the DXL IMU board. It starts the newest confirmed application slot, or stays on the bus
   (USART3, and the USART2 soft bus) to receive a new image when the application asked for it, when no slot
   can be started, or after a reset into an update request. Selection and the update protocol are the
   portable code in src/ota.c; this file only provides flash, serial ports and the jump. */
#include "ch32v20x.h"
#include "ch32v20x_flash.h"
#include "ch32v20x_rcc.h"
#include "ch32v20x_usart.h"
#include "imu_power/config.h"
#include "imu_power/ota.h"

#include <string.h>

/* Written by the application before it resets into the bootloader (see platform/ch32v203_port.c). */
#define BOOT_REQUEST_ADDRESS 0x20004FF0u
#define BOOT_REQUEST_MAGIC 0x4F544131u
#define IDLE_RESTART_TICKS (60u * 12000000u) /* 60 s of SysTick at HCLK/8 */
#define FRAME_TIMEOUT_TICKS (30u * 12000u)
#define REPLY_DELAY_TICKS 12000u /* 1 ms */

__attribute__((section(".bootinfo"), used))
static const uint32_t boot_info[4] = {
    IMU_POWER_OTA_BOOT_INFO_MAGIC,
    (uint32_t)IMU_POWER_OTA_BOOT_VERSION | ((uint32_t)IMU_POWER_OTA_HWID << 16u), 0u, 0u
};

static void flash_read(void *context, uint32_t address, void *dst, size_t size)
{
    (void)context;
    memcpy(dst, (const void *)address, size);
}

static bool flash_erase(void *context, uint32_t page)
{
    FLASH_Status status;
    (void)context;
    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_WRPRTERR);
    status = FLASH_ErasePage(page);
    FLASH_Lock();
    return status == FLASH_COMPLETE;
}

static bool flash_program(void *context, uint32_t address, const void *src, size_t size)
{
    const uint8_t *data = (const uint8_t *)src;
    size_t i;
    bool ok = true;
    (void)context;
    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_WRPRTERR);
    if (size == IMU_POWER_OTA_BLOCK && (address & (IMU_POWER_OTA_BLOCK - 1u)) == 0u) {
        /* A 256-byte block in fast page mode: about a hundred times quicker than half-words. */
        FLASH_Unlock_Fast();
        FLASH_ProgramPage_Fast(address, (uint32_t *)(uintptr_t)src);
        FLASH_Lock_Fast();
        ok = memcmp((const void *)address, src, size) == 0;
    } else {
        for (i = 0; ok && i < size; i += 2u) {
            ok = FLASH_ProgramHalfWord(address + (uint32_t)i,
                                       (uint16_t)data[i] | ((uint16_t)data[i + 1u] << 8u)) == FLASH_COMPLETE;
        }
    }
    FLASH_Lock();
    return ok;
}

static const imu_power_ota_flash_t flash = { NULL, flash_read, flash_erase, flash_program };

static bool config_read(void *context, unsigned slot, void *dst, size_t size)
{
    (void)context;
    if (slot > 1u || size > 4096u) return false;
    memcpy(dst, (const void *)(slot == 0u ? 0x0800E000u : 0x0800F000u), size);
    return true;
}

static void start_application(unsigned slot)
{
    uint32_t entry = IMU_POWER_OTA_SLOT_ADDRESS(slot) + IMU_POWER_OTA_HEADER_SIZE;
    /* Hand over the peripherals in their reset state. */
    RCC->APB2PRSTR = RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB | RCC_APB2Periph_AFIO;
    RCC->APB1PRSTR = RCC_APB1Periph_USART2 | RCC_APB1Periph_USART3;
    RCC->APB2PRSTR = 0u;
    RCC->APB1PRSTR = 0u;
    __asm volatile("csrw mstatus, zero\n jr %0" : : "r"(entry));
    for (;;) {}
}

/* Pin mode nibble: MODE[1:0] output speed (0 = input), CNF[1:0]. */
static void pin_mode(GPIO_TypeDef *port, unsigned pin, uint32_t nibble)
{
    volatile uint32_t *reg = pin < 8u ? &port->CFGLR : &port->CFGHR;
    unsigned shift = (pin & 7u) * 4u;
    *reg = (*reg & ~(0xFu << shift)) | (nibble << shift);
}

static void usart_start(USART_TypeDef *port, uint32_t pclk, uint32_t baud)
{
    port->CTLR1 = 0u; port->CTLR2 = 0u; port->CTLR3 = 0u;
    port->BRR = (pclk + baud / 2u) / baud;
    port->CTLR1 = USART_CTLR1_UE | USART_CTLR1_TE | USART_CTLR1_RE;
}

static void serial_init(uint32_t bus_baud)
{
    uint32_t pre = (RCC->CFGR0 >> 8u) & 7u; /* PPRE1 */
    uint32_t pclk1 = SystemCoreClock >> (pre >= 4u ? pre - 3u : 0u);
    RCC->APB2PCENR |= RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB | RCC_APB2Periph_AFIO;
    RCC->APB1PCENR |= RCC_APB1Periph_USART2 | RCC_APB1Periph_USART3;
    pin_mode(GPIOB, 10u, 0xFu); /* USART3 TX, alternate open drain */
    pin_mode(GPIOB, 11u, 0x4u); /* USART3 RX, floating */
    pin_mode(GPIOA, 15u, 0x3u); /* bus direction, low = receive */
    GPIOA->BCR = 1u << 15u;
    pin_mode(GPIOA, 2u, 0xBu);  /* USART2 TX, alternate push-pull */
    pin_mode(GPIOA, 3u, 0x8u);  /* USART2 RX with pull-up: an unconnected adapter stays quiet */
    GPIOA->BSHR = 1u << 3u;
    usart_start(USART3, pclk1, bus_baud);
    usart_start(USART2, pclk1, 1000000u);
}

static void transmit(USART_TypeDef *port, const uint8_t *data, size_t length)
{
    if (port == USART3) GPIOA->BSHR = 1u << 15u;
    while (length-- != 0u) {
        while ((port->STATR & USART_FLAG_TXE) == 0u) {}
        port->DATAR = *data++;
    }
    while ((port->STATR & USART_FLAG_TC) == 0u) {}
    if (port == USART3) GPIOA->BCR = 1u << 15u;
}

static bool startable(unsigned slot)
{
    imu_power_ota_slot_t info;
    imu_power_ota_slot_read(&flash, slot, true, &info);
    return info.header_ok && info.payload_ok && !info.bad && info.committed;
}

static imu_power_dxl_stream_t streams[2];
static uint32_t last_byte[2];
static uint8_t scratch[IMU_POWER_DXL_MAX_PACKET];
static uint8_t response[96];

static uint32_t ticks(void)
{
    return (uint32_t)SysTick->CNT;
}

static void serve(imu_power_ota_t *ota, bool can_leave)
{
    USART_TypeDef *const ports[2] = { USART3, USART2 };
    uint32_t last_frame = ticks();
    for (;;) {
        unsigned i;
        for (i = 0; i < 2u; ++i) {
            USART_TypeDef *port = ports[i];
            if ((port->STATR & (USART_FLAG_RXNE | USART_FLAG_ORE)) != 0u) {
                imu_power_dxl_frame_t frame;
                uint8_t byte = (uint8_t)port->DATAR;
                last_byte[i] = ticks();
                if (imu_power_dxl_stream_feed(&streams[i], byte, &frame, scratch, sizeof(scratch))) {
                    size_t n = imu_power_ota_handle(ota, &frame, response, sizeof(response));
                    last_frame = ticks();
                    if (n != 0u) {
                        /* Return delay: a master (and the bench bridge between it and the bus) needs a moment
                           to finish its own transmission before a reply can be told from an echo. */
                        uint32_t t = ticks();
                        while ((uint32_t)(ticks() - t) < REPLY_DELAY_TICKS) {}
                        transmit(port, response, n);
                    }
                    if (ota->reboot_requested) NVIC_SystemReset();
                }
            } else if (streams[i].length != 0u && (uint32_t)(ticks() - last_byte[i]) > FRAME_TIMEOUT_TICKS) {
                streams[i].length = 0u;
            }
        }
        /* Nobody finished an update: go back to the application instead of waiting for ever. */
        if (can_leave && ota->state == 0u && (uint32_t)(ticks() - last_frame) > IDLE_RESTART_TICKS)
            NVIC_SystemReset();
    }
}

int main(void)
{
    volatile uint32_t *request = (volatile uint32_t *)BOOT_REQUEST_ADDRESS;
    imu_power_config_storage_t storage = { config_read, NULL, NULL };
    imu_power_config_t config;
    imu_power_ota_t ota;
    uint32_t generation;
    bool stay = request[0] == BOOT_REQUEST_MAGIC && request[1] == ~BOOT_REQUEST_MAGIC;
    int slot = -1;
    request[0] = request[1] = 0u;
    if (!stay) {
        slot = imu_power_ota_select(&flash);
        if (slot >= 0) start_application((unsigned)slot);
    }
    if (!imu_power_config_load(&storage, &config, &generation)) imu_power_config_defaults(&config);
    SysTick->CTLR = 1u; /* free-running counter at HCLK/8 */
    serial_init(config.baudrate);
    imu_power_ota_init(&ota, &flash, config.device_id);
    /* Only an application that can be started again justifies an idle timeout. */
    serve(&ota, startable(0u) || startable(1u));
    return 0;
}
