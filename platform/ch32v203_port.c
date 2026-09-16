#include "ch32v203_port.h"

#ifdef IMU_POWER_CH32

#include "ch32v20x_flash.h"
#include "ch32v20x_gpio.h"
#include "ch32v20x_spi.h"
#include "ch32v20x_usart.h"
#include "ch32v20x_dma.h"
#include "ch32v20x_exti.h"
#include "ch32v20x_iwdg.h"
#include "ch32v20x_misc.h"
#include "imu_power/maintenance.h"

#include <string.h>

#define CONFIG_SLOT0_ADDR 0x0800E000u
#define CONFIG_SLOT1_ADDR 0x0800F000u
#define CONFIG_PAGE_SIZE 4096u
#define DXL_RX_SIZE IMU_POWER_CH32_RX_SIZE
#define DXL_TX_SIZE 512u
#define IMU_REG_WHO_AM_I 0x0Fu
#define IMU_REG_CTRL1_XL 0x10u
#define IMU_REG_CTRL2_G 0x11u
#define IMU_REG_CTRL3_C 0x12u
#define IMU_REG_FUNC_CFG_ACCESS 0x01u
#define IMU_REG_FIFO_CTRL1 0x07u
#define IMU_REG_FIFO_CTRL2 0x08u
#define IMU_REG_FIFO_CTRL3 0x09u
#define IMU_REG_FIFO_CTRL4 0x0Au
#define IMU_REG_EMB_FUNC_EN_A 0x04u
#define IMU_REG_EMB_FUNC_FIFO_EN_A 0x44u
#define IMU_REG_SFLP_ODR 0x5Eu
#define IMU_REG_OUTX_L_G 0x22u
#define IMU_REG_FIFO_STATUS1 0x1Bu
#define IMU_REG_FIFO_STATUS2 0x1Cu
#define IMU_REG_FIFO_DATA_OUT_TAG 0x78u
#define IMU_WHO_AM_I 0x70u

static uint8_t dxl_dma_rx[DXL_RX_SIZE];
static uint16_t dxl_rx_tail;
static uint8_t dxl_packet[DXL_RX_SIZE];
static uint8_t dxl_scratch[DXL_RX_SIZE];
static uint8_t dxl_response[DXL_TX_SIZE];
static uint16_t dxl_packet_len;
static uint32_t dxl_last_rx_ms;
static volatile uint32_t systick_ms;
static uint8_t imu_ready;
static char debug_line[128];
static uint16_t debug_line_len;
static uint32_t last_sample_ms;
static uint32_t last_recovery_ms;
static volatile uint8_t imu_sample_pending;

void SysTick_Handler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void SysTick_Handler(void)
{
    SysTick->SR = 0u;
    ++systick_ms;
}

void EXTI15_10_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void EXTI15_10_IRQHandler(void)
{
    if (EXTI_GetITStatus(EXTI_Line14) != RESET) {
        imu_sample_pending = 1u;
        EXTI_ClearITPendingBit(EXTI_Line14);
    }
}

static bool flash_read(void *context, unsigned slot, void *dst, size_t size)
{
    const uint8_t *source;
    size_t i;
    (void)context;
    if (slot > 1u || size > CONFIG_PAGE_SIZE) return false;
    source = (const uint8_t *)(slot == 0u ? CONFIG_SLOT0_ADDR : CONFIG_SLOT1_ADDR);
    for (i = 0; i < size; ++i) ((uint8_t *)dst)[i] = source[i];
    return true;
}

static bool flash_write(void *context, unsigned slot, const void *src, size_t size)
{
    uint32_t address = slot == 0u ? CONFIG_SLOT0_ADDR : CONFIG_SLOT1_ADDR;
    const uint8_t *data = (const uint8_t *)src;
    size_t i;
    (void)context;
    if (slot > 1u || size > CONFIG_PAGE_SIZE || (size & 1u) != 0u) return false;
    FLASH_Unlock();
    FLASH_ClearFlag(FLASH_FLAG_EOP | FLASH_FLAG_WRPRTERR);
    if (FLASH_ErasePage(address) != FLASH_COMPLETE) { FLASH_Lock(); return false; }
    for (i = 0; i < size; i += 2u) {
        uint16_t word = (uint16_t)data[i] | ((uint16_t)data[i + 1u] << 8u);
        if (FLASH_ProgramHalfWord(address + (uint32_t)i, word) != FLASH_COMPLETE) {
            FLASH_Lock(); return false;
        }
    }
    FLASH_Lock();
    return true;
}

static void dxl_direction(bool transmit)
{
    if (transmit) GPIO_SetBits(GPIOA, GPIO_Pin_15);
    else GPIO_ResetBits(GPIOA, GPIO_Pin_15);
}

static void spi_cs(bool asserted)
{
    if (asserted) GPIO_ResetBits(GPIOB, GPIO_Pin_0);
    else GPIO_SetBits(GPIOB, GPIO_Pin_0);
}

static uint8_t spi_transfer(uint8_t value)
{
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_TXE) == RESET) {}
    SPI_I2S_SendData(SPI1, value);
    while (SPI_I2S_GetFlagStatus(SPI1, SPI_I2S_FLAG_RXNE) == RESET) {}
    return (uint8_t)SPI_I2S_ReceiveData(SPI1);
}

static uint8_t imu_read_reg(uint8_t reg)
{
    uint8_t value;
    spi_cs(true); spi_transfer((uint8_t)(reg | 0x80u)); value = spi_transfer(0u); spi_cs(false);
    return value;
}

static void imu_write_reg(uint8_t reg, uint8_t value)
{
    spi_cs(true); spi_transfer((uint8_t)(reg & 0x7Fu)); spi_transfer(value); spi_cs(false);
}

static void imu_read_burst(uint8_t reg, uint8_t *data, size_t length)
{
    size_t i;
    spi_cs(true); spi_transfer((uint8_t)(reg | 0x80u));
    for (i = 0; i < length; ++i) data[i] = spi_transfer(0u);
    spi_cs(false);
}

static void imu_delay_ms(uint32_t delay_ms)
{
    uint32_t start = systick_ms;
    while ((uint32_t)(systick_ms - start) < delay_ms) {}
}

static bool imu_init(void)
{
    uint8_t who;
    who = imu_read_reg(IMU_REG_WHO_AM_I);
    if (who != IMU_WHO_AM_I) return false;
    imu_write_reg(IMU_REG_FUNC_CFG_ACCESS, 0x04u); /* software power-on reset */
    imu_delay_ms(30u);
    if (imu_read_reg(IMU_REG_WHO_AM_I) != IMU_WHO_AM_I) return false;
    imu_write_reg(IMU_REG_CTRL3_C, 0x44u); /* BDU + IF_INC */
    imu_write_reg(IMU_REG_CTRL1_XL, 0x04u); /* 30 Hz, high performance */
    imu_write_reg(IMU_REG_CTRL2_G, 0x04u); /* 30 Hz, high performance */
    imu_write_reg(IMU_REG_FUNC_CFG_ACCESS, 0x80u); /* embedded-function bank */
    imu_write_reg(IMU_REG_EMB_FUNC_FIFO_EN_A, 0x02u); /* game rotation to FIFO */
    imu_write_reg(IMU_REG_SFLP_ODR, 0x08u); /* SFLP game rotation at 30 Hz */
    imu_write_reg(IMU_REG_EMB_FUNC_EN_A, 0x02u); /* enable game rotation */
    imu_write_reg(IMU_REG_FUNC_CFG_ACCESS, 0x00u); /* main bank */
    imu_write_reg(IMU_REG_FIFO_CTRL1, 0x01u);
    imu_write_reg(IMU_REG_FIFO_CTRL2, 0x00u);
    imu_write_reg(IMU_REG_FIFO_CTRL3, 0x00u);
    imu_write_reg(IMU_REG_FIFO_CTRL4, 0x06u);
    return true;
}

static void sample_update(imu_power_device_t *device)
{
    uint8_t bytes[6];
    uint8_t fifo[7];
    imu_power_sample_t sample;
    static uint32_t counter;
    sample = device->sample;
    imu_read_burst(IMU_REG_OUTX_L_G, bytes, sizeof(bytes));
    sample.gyro_raw[0] = (int16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8u));
    sample.gyro_raw[1] = (int16_t)((uint16_t)bytes[2] | ((uint16_t)bytes[3] << 8u));
    sample.gyro_raw[2] = (int16_t)((uint16_t)bytes[4] | ((uint16_t)bytes[5] << 8u));
    if ((imu_read_reg(IMU_REG_FIFO_STATUS2) & 0x01u) != 0u ||
        imu_read_reg(IMU_REG_FIFO_STATUS1) != 0u) {
        imu_read_burst(IMU_REG_FIFO_DATA_OUT_TAG, fifo, sizeof(fifo));
        if ((fifo[0] >> 3u) == 0x13u) {
            sample.quat_half[0] = (uint16_t)fifo[1] | ((uint16_t)fifo[2] << 8u);
            sample.quat_half[1] = (uint16_t)fifo[3] | ((uint16_t)fifo[4] << 8u);
            sample.quat_half[2] = (uint16_t)fifo[5] | ((uint16_t)fifo[6] << 8u);
        }
    }
    sample.sample_counter = ++counter;
    sample.age_us = 0u;
    sample.valid = imu_power_quaternion_valid(sample.quat_half);
    imu_power_device_set_sample(device, &sample);
}

static void gpio_uart_spi_init(uint32_t baudrate)
{
    GPIO_InitTypeDef gpio = {0};
    USART_InitTypeDef uart = {0};
    SPI_InitTypeDef spi = {0};
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB | RCC_APB2Periph_SPI1 |
                           RCC_APB2Periph_AFIO, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART3, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    AFIO->PCFR1 = (AFIO->PCFR1 & ~AFIO_PCFR1_SWJ_CFG) | AFIO_PCFR1_SWJ_CFG_JTAGDISABLE;
    gpio.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_7; gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz; GPIO_Init(GPIOA, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_6; gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING; GPIO_Init(GPIOA, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_0; gpio.GPIO_Mode = GPIO_Mode_Out_PP; GPIO_Init(GPIOB, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_14 | GPIO_Pin_15; gpio.GPIO_Mode = GPIO_Mode_IPU; GPIO_Init(GPIOB, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_10; gpio.GPIO_Mode = GPIO_Mode_AF_OD; GPIO_Init(GPIOB, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_11; gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING; GPIO_Init(GPIOB, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_15; gpio.GPIO_Mode = GPIO_Mode_Out_PP; GPIO_Init(GPIOA, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_9; gpio.GPIO_Mode = GPIO_Mode_AF_PP; GPIO_Init(GPIOA, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_10; gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING; GPIO_Init(GPIOA, &gpio);
    GPIO_EXTILineConfig(GPIO_PortSourceGPIOB, GPIO_PinSource14);
    {
        EXTI_InitTypeDef exti = {0};
        NVIC_InitTypeDef nvic = {0};
        exti.EXTI_Line = EXTI_Line14; exti.EXTI_Mode = EXTI_Mode_Interrupt;
        exti.EXTI_Trigger = EXTI_Trigger_Rising; exti.EXTI_LineCmd = ENABLE;
        EXTI_Init(&exti);
        nvic.NVIC_IRQChannel = EXTI15_10_IRQn;
        nvic.NVIC_IRQChannelPreemptionPriority = 1u;
        nvic.NVIC_IRQChannelSubPriority = 1u;
        nvic.NVIC_IRQChannelCmd = ENABLE;
        NVIC_Init(&nvic);
    }
    spi.SPI_Direction = SPI_Direction_2Lines_FullDuplex; spi.SPI_Mode = SPI_Mode_Master;
    spi.SPI_DataSize = SPI_DataSize_8b; spi.SPI_CPOL = SPI_CPOL_High; spi.SPI_CPHA = SPI_CPHA_2Edge;
    spi.SPI_NSS = SPI_NSS_Soft; spi.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_16;
    spi.SPI_FirstBit = SPI_FirstBit_MSB; spi.SPI_CRCPolynomial = 7u; SPI_Init(SPI1, &spi); SPI_Cmd(SPI1, ENABLE);
    uart.USART_BaudRate = baudrate; uart.USART_WordLength = USART_WordLength_8b;
    uart.USART_StopBits = USART_StopBits_1; uart.USART_Parity = USART_Parity_No;
    uart.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    uart.USART_Mode = USART_Mode_Tx | USART_Mode_Rx; USART_Init(USART3, &uart); USART_Cmd(USART3, ENABLE);
    uart.USART_BaudRate = 115200u; USART_Init(USART1, &uart); USART_Cmd(USART1, ENABLE);
    {
        DMA_InitTypeDef dma = {0};
        DMA_DeInit(DMA1_Channel3);
        dma.DMA_PeripheralBaseAddr = (uint32_t)&USART3->DATAR;
        dma.DMA_MemoryBaseAddr = (uint32_t)dxl_dma_rx;
        dma.DMA_DIR = DMA_DIR_PeripheralSRC;
        dma.DMA_BufferSize = DXL_RX_SIZE;
        dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
        dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
        dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
        dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
        dma.DMA_Mode = DMA_Mode_Circular;
        dma.DMA_Priority = DMA_Priority_VeryHigh;
        dma.DMA_M2M = DMA_M2M_Disable;
        DMA_Init(DMA1_Channel3, &dma);
        DMA_Cmd(DMA1_Channel3, ENABLE);
        USART_DMACmd(USART3, USART_DMAReq_Rx, ENABLE);
    }
    dxl_direction(false); spi_cs(false);
}

static void debug_write(const char *text)
{
    while (*text != '\0') {
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) {}
        USART_SendData(USART1, (uint8_t)*text++);
    }
    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET) {}
}

static void debug_poll(imu_power_device_t *device)
{
    while (USART_GetFlagStatus(USART1, USART_FLAG_RXNE) != RESET) {
        char c = (char)USART_ReceiveData(USART1);
        if (c == '\r' || c == '\n') {
            char output[192];
            if (debug_line_len != 0u) {
                debug_line[debug_line_len] = '\0';
                imu_power_cli_execute(device, debug_line, output, sizeof(output));
                debug_write(output);
                debug_line_len = 0u;
            }
        } else if (debug_line_len + 1u < sizeof(debug_line)) {
            debug_line[debug_line_len++] = c;
        } else {
            debug_line_len = 0u;
        }
    }
}

void imu_power_ch32_port_init(imu_power_device_t *device)
{
    imu_power_config_storage_t storage = { flash_read, flash_write, NULL };
    SystemCoreClockUpdate();
    SysTick->SR = 0u;
    SysTick->CNT = 0u;
    SysTick->CMP = SystemCoreClock / 1000u;
    /* Enable counter, interrupt, HCLK source, and automatic reload. */
    SysTick->CTLR = 0x0Fu;
    NVIC_EnableIRQ(SysTick_IRQn);
    imu_power_device_init(device, &storage);
    if (RCC_GetFlagStatus(RCC_FLAG_IWDGRST) != RESET) device->diagnostics.reset_reason |= 1u;
    if (RCC_GetFlagStatus(RCC_FLAG_SFTRST) != RESET) device->diagnostics.reset_reason |= 2u;
    RCC_ClearFlag();
    gpio_uart_spi_init(device->active_config.baudrate);
    IWDG_WriteAccessCmd(IWDG_WriteAccess_Enable);
    IWDG_SetPrescaler(IWDG_Prescaler_64);
    IWDG_SetReload(2500u);
    IWDG_ReloadCounter();
    IWDG_Enable();
    imu_ready = imu_init() ? 1u : 0u;
    if (imu_ready) device->diagnostics.status_flags |= (1u << 1);
    else device->diagnostics.status_flags |= (1u << 2);
}

void imu_power_ch32_port_poll(imu_power_device_t *device)
{
    debug_poll(device);
    {
        uint16_t dma_head = imu_power_ch32_rx_head((uint16_t)DMA1_Channel3->CNTR);
        while (dxl_rx_tail != dma_head) {
            uint8_t value = dxl_dma_rx[dxl_rx_tail];
            dxl_rx_tail = (uint16_t)((dxl_rx_tail + 1u) % DXL_RX_SIZE);
            dxl_last_rx_ms = systick_ms;
            dxl_packet[dxl_packet_len++] = value;
            if ((dxl_packet_len == 1u && dxl_packet[0] != 0xFFu) ||
                (dxl_packet_len == 2u && dxl_packet[1] != 0xFFu) ||
                (dxl_packet_len == 3u && dxl_packet[2] != 0xFDu) ||
                (dxl_packet_len == 4u && dxl_packet[3] != 0u)) {
                dxl_packet_len = 0u;
                continue;
            }
            if (dxl_packet_len >= 10u) {
                imu_power_dxl_frame_t frame;
                if (imu_power_dxl_parse(dxl_packet, dxl_packet_len, &frame, dxl_scratch, sizeof(dxl_scratch))) {
                    size_t response_length = imu_power_device_handle(device, &frame, dxl_response, sizeof(dxl_response));
                    if (response_length != 0u) {
                        DMA_InitTypeDef tx = {0};
                        dxl_direction(true);
                        DMA_DeInit(DMA1_Channel2);
                        tx.DMA_PeripheralBaseAddr = (uint32_t)&USART3->DATAR;
                        tx.DMA_MemoryBaseAddr = (uint32_t)dxl_response;
                        tx.DMA_DIR = DMA_DIR_PeripheralDST;
                        tx.DMA_BufferSize = (uint16_t)response_length;
                        tx.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
                        tx.DMA_MemoryInc = DMA_MemoryInc_Enable;
                        tx.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
                        tx.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
                        tx.DMA_Mode = DMA_Mode_Normal;
                        tx.DMA_Priority = DMA_Priority_VeryHigh;
                        tx.DMA_M2M = DMA_M2M_Disable;
                        DMA_Init(DMA1_Channel2, &tx);
                        DMA_ClearFlag(DMA1_FLAG_TC2);
                        USART_DMACmd(USART3, USART_DMAReq_Tx, ENABLE);
                        DMA_Cmd(DMA1_Channel2, ENABLE);
                        while (DMA_GetFlagStatus(DMA1_FLAG_TC2) == RESET) {}
                        while (USART_GetFlagStatus(USART3, USART_FLAG_TC) == RESET) {}
                        DMA_Cmd(DMA1_Channel2, DISABLE);
                        USART_DMACmd(USART3, USART_DMAReq_Tx, DISABLE);
                        dxl_direction(false);
                    }
                    dxl_packet_len = 0u;
                }
            }
            if (dxl_packet_len >= DXL_RX_SIZE) dxl_packet_len = 0u;
        }
    }
    if (dxl_packet_len != 0u && (uint32_t)(systick_ms - dxl_last_rx_ms) > 30u) {
        dxl_packet_len = 0u;
        ++device->diagnostics.error_count;
        device->diagnostics.last_error = IMU_POWER_DXL_DATA_LENGTH;
    }
    if (imu_ready && (imu_sample_pending ||
        (systick_ms != last_sample_ms && (systick_ms % 10u) == 0u))) {
        imu_sample_pending = 0u;
        last_sample_ms = systick_ms;
        sample_update(device);
    }
    if (!imu_ready && systick_ms != last_recovery_ms && (systick_ms % 1000u) == 0u) {
        last_recovery_ms = systick_ms;
        imu_ready = imu_init() ? 1u : 0u;
        if (imu_ready) {
            device->diagnostics.status_flags |= (1u << 1);
            device->diagnostics.status_flags &= (uint16_t)~(1u << 2);
        } else {
            device->diagnostics.status_flags |= (1u << 2);
        }
    }
    if (device->reboot_requested) imu_power_ch32_port_request_reboot();
}

void imu_power_ch32_port_request_reboot(void)
{
    NVIC_SystemReset();
}

#else
int imu_power_ch32v203_port_requires_wch_evt;
#endif
