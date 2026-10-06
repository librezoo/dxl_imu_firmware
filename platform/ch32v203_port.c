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
#include "imu_power/softbus.h"
#include "imu_power/maintenance.h"

#include <string.h>

#define CONFIG_SLOT0_ADDR 0x0800E000u
#define CONFIG_SLOT1_ADDR 0x0800F000u
#define CONFIG_PAGE_SIZE 4096u
#define DXL_RX_SIZE IMU_POWER_CH32_RX_SIZE
#define DXL_TX_SIZE 512u
#define SOFTBUS_BAUDRATE 1000000u
#define IMU_REG_WHO_AM_I 0x0Fu
#define IMU_REG_CTRL1_XL 0x10u
#define IMU_REG_CTRL2_G 0x11u
#define IMU_REG_CTRL3_C 0x12u
#define IMU_REG_CTRL6 0x15u
#define IMU_REG_CTRL8 0x17u
#define IMU_REG_STATUS 0x1Eu
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
static imu_power_dxl_stream_t dxl_rx;
static uint8_t dxl_scratch[DXL_RX_SIZE];
static uint8_t dxl_response[DXL_TX_SIZE];
static uint32_t dxl_last_rx_ms;
static volatile uint32_t systick_ms;
static uint8_t imu_ready;
static char debug_line[128];
static uint16_t debug_line_len;
static imu_power_softbus_t softbus;
static uint8_t softbus_dma_rx[IMU_POWER_SOFTBUS_MAX_BURST];
static volatile uint16_t softbus_burst_length;
static volatile uint8_t softbus_burst_ready;
static volatile uint8_t softbus_burst_overflow;
static uint8_t softbus_tx_dma_active;
static uint16_t softbus_tx_dma_length;
static uint8_t softbus_injection_pending_end;
static uint32_t last_sample_ms;
static uint32_t last_recovery_ms;
static volatile uint8_t imu_sample_pending;
static imu_power_sample_clock_t imu_sample_clock;

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

void USART2_IRQHandler(void) __attribute__((interrupt("WCH-Interrupt-fast")));
void USART2_IRQHandler(void)
{
    uint32_t status = USART2->STATR;
    if ((status & USART_FLAG_IDLE) != 0u) {
        uint16_t remaining;
        DMA_Cmd(DMA1_Channel6, DISABLE);
        USART_DMACmd(USART2, USART_DMAReq_Rx, DISABLE);
        remaining = DMA_GetCurrDataCounter(DMA1_Channel6);
        /* IDLE/error flags are cleared by reading STATR followed by DATAR. */
        (void)USART2->DATAR;
        if (softbus_burst_ready ||
            (status & (USART_FLAG_ORE | USART_FLAG_NE | USART_FLAG_FE)) != 0u ||
            (remaining == 0u && (status & USART_FLAG_RXNE) != 0u)) {
            softbus_burst_overflow = 1u;
        }
        softbus_burst_length = (uint16_t)(IMU_POWER_SOFTBUS_MAX_BURST - remaining);
        softbus_burst_ready = 1u;
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

static bool imu_init(imu_power_device_t *device)
{
    uint8_t who;
    uint32_t counter = device->sample.sample_counter;
    memset(&imu_sample_clock, 0, sizeof(imu_sample_clock));
    memset(&device->sample, 0, sizeof(device->sample));
    device->sample.sample_counter = counter;
    device->sample.age_us = UINT32_MAX;
    device->sample.gyro_ratio = (uint8_t)imu_power_config_gyro_ratio(device->runtime_gyro_fs);
    imu_power_device_set_sample(device, &device->sample);
    who = imu_read_reg(IMU_REG_WHO_AM_I);
    if (who != IMU_WHO_AM_I) return false;
    imu_write_reg(IMU_REG_FUNC_CFG_ACCESS, 0x04u); /* software power-on reset */
    imu_delay_ms(30u);
    if (imu_read_reg(IMU_REG_WHO_AM_I) != IMU_WHO_AM_I) return false;
    imu_write_reg(IMU_REG_CTRL3_C, 0x44u); /* BDU + IF_INC */
    /* Defaults are +/-500 dps (robotd uses 17.5 mdps/LSB), +/-2 g, 240 Hz;
       the bus-writable configuration can change them. */
    imu_write_reg(IMU_REG_CTRL6, device->runtime_gyro_fs);
    imu_write_reg(IMU_REG_CTRL8, device->runtime_accel_fs);
    imu_write_reg(IMU_REG_CTRL1_XL, device->runtime_imu_odr); /* high performance */
    imu_write_reg(IMU_REG_CTRL2_G, device->runtime_imu_odr);
    imu_write_reg(IMU_REG_FUNC_CFG_ACCESS, 0x80u); /* embedded-function bank */
    imu_write_reg(IMU_REG_EMB_FUNC_FIFO_EN_A, 0x22u); /* game rotation + gyro bias */
    /* SFLP_ODR bits 6,1,0 must stay one (datasheet table 323). Update only
       ODR[5:3]; a whole-byte 0x18 write incorrectly clears the reset bits. */
    imu_write_reg(IMU_REG_SFLP_ODR, (uint8_t)((imu_read_reg(IMU_REG_SFLP_ODR) & 0xC7u) |
                                              (device->runtime_sflp_odr << 3u)));
    imu_write_reg(IMU_REG_EMB_FUNC_EN_A, 0x02u); /* enable game rotation */
    imu_write_reg(IMU_REG_FUNC_CFG_ACCESS, 0x00u); /* main bank */
    imu_write_reg(IMU_REG_FIFO_CTRL1, 0x01u);
    imu_write_reg(IMU_REG_FIFO_CTRL2, 0x00u);
    imu_write_reg(IMU_REG_FIFO_CTRL3, 0x00u);
    imu_write_reg(IMU_REG_FIFO_CTRL4, 0x06u);
    /* Observe the physical configuration, not just the values we intended
       to write. The readonly diagnostic tail leaves the 12-byte sample intact. */
    device->diagnostics.imu_registers[0] = imu_read_reg(IMU_REG_WHO_AM_I);
    device->diagnostics.imu_registers[1] = imu_read_reg(IMU_REG_CTRL1_XL);
    device->diagnostics.imu_registers[2] = imu_read_reg(IMU_REG_CTRL2_G);
    device->diagnostics.imu_registers[3] = imu_read_reg(IMU_REG_CTRL3_C);
    device->diagnostics.imu_registers[4] = imu_read_reg(IMU_REG_CTRL6);
    device->diagnostics.imu_registers[5] = imu_read_reg(IMU_REG_CTRL8);
    imu_write_reg(IMU_REG_FUNC_CFG_ACCESS, 0x80u);
    device->diagnostics.imu_registers[6] = imu_read_reg(IMU_REG_EMB_FUNC_EN_A);
    device->diagnostics.imu_registers[7] = imu_read_reg(IMU_REG_SFLP_ODR);
    imu_write_reg(IMU_REG_FUNC_CFG_ACCESS, 0x00u);
    return device->diagnostics.imu_registers[0] == IMU_WHO_AM_I &&
           device->diagnostics.imu_registers[1] == device->runtime_imu_odr &&
           device->diagnostics.imu_registers[2] == device->runtime_imu_odr &&
           (device->diagnostics.imu_registers[3] & 0x44u) == 0x44u &&
           (device->diagnostics.imu_registers[4] & 0x0Fu) == device->runtime_gyro_fs &&
           (device->diagnostics.imu_registers[5] & 0x03u) == device->runtime_accel_fs &&
           (device->diagnostics.imu_registers[6] & 0x02u) != 0u &&
           device->diagnostics.imu_registers[7] ==
               (uint8_t)(0x43u | (device->runtime_sflp_odr << 3u));
}

static void sample_update(imu_power_device_t *device)
{
    uint8_t bytes[6];
    uint8_t fifo[7];
    uint8_t fifo_status[2];
    uint16_t count;
    imu_power_sample_t sample;
    sample = device->sample;
    if ((imu_read_reg(IMU_REG_STATUS) & 0x02u) != 0u) {
        imu_read_burst(IMU_REG_OUTX_L_G, bytes, sizeof(bytes));
        imu_power_sample_gyro(&sample, &imu_sample_clock, bytes, systick_ms);
    }
    imu_read_burst(IMU_REG_FIFO_STATUS1, fifo_status, sizeof(fifo_status));
    count = (uint16_t)fifo_status[0] | ((uint16_t)(fifo_status[1] & 1u) << 8u);
    if ((fifo_status[1] & 0x48u) != 0u)
        device->diagnostics.status_flags |= IMU_POWER_STATUS_IMU_FIFO_FAULT;
    /* Drain the bounded hardware FIFO so a polling delay cannot make us
       publish an old quaternion while claiming it was just sampled. */
    while (count-- != 0u) {
        imu_read_burst(IMU_REG_FIFO_DATA_OUT_TAG, fifo, sizeof(fifo));
        imu_power_sample_fifo(&sample, &imu_sample_clock, fifo, systick_ms);
    }
    imu_power_sample_age(&sample, &imu_sample_clock, systick_ms);
    imu_power_device_set_sample(device, &sample);
}

static void softbus_rx_start(void)
{
    DMA_Cmd(DMA1_Channel6, DISABLE);
    DMA_ClearFlag(DMA1_FLAG_GL6);
    DMA_SetCurrDataCounter(DMA1_Channel6, IMU_POWER_SOFTBUS_MAX_BURST);
    softbus_burst_length = 0u;
    softbus_burst_ready = 0u;
    softbus_burst_overflow = 0u;
    USART_DMACmd(USART2, USART_DMAReq_Rx, ENABLE);
    DMA_Cmd(DMA1_Channel6, ENABLE);
}

static void gpio_uart_spi_init(uint32_t baudrate)
{
    GPIO_InitTypeDef gpio = {0};
    USART_InitTypeDef uart = {0};
    SPI_InitTypeDef spi = {0};
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB | RCC_APB2Periph_SPI1 |
                           RCC_APB2Periph_AFIO, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_USART2 | RCC_APB1Periph_USART3, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART1, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);
    AFIO->PCFR1 = (AFIO->PCFR1 & ~AFIO_PCFR1_SWJ_CFG) | AFIO_PCFR1_SWJ_CFG_JTAGDISABLE;
    gpio.GPIO_Pin = GPIO_Pin_5 | GPIO_Pin_7; gpio.GPIO_Mode = GPIO_Mode_AF_PP;
    gpio.GPIO_Speed = GPIO_Speed_50MHz; GPIO_Init(GPIOA, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_6; gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING; GPIO_Init(GPIOA, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_0; gpio.GPIO_Mode = GPIO_Mode_Out_PP; GPIO_Init(GPIOB, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_14 | GPIO_Pin_15; gpio.GPIO_Mode = GPIO_Mode_IPU; GPIO_Init(GPIOB, &gpio);
    /* USART3 is the physical half-duplex DXL bus behind the PA15 enable. */
    gpio.GPIO_Pin = GPIO_Pin_10; gpio.GPIO_Mode = GPIO_Mode_AF_OD; GPIO_Init(GPIOB, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_11; gpio.GPIO_Mode = GPIO_Mode_IN_FLOATING; GPIO_Init(GPIOB, &gpio);
    gpio.GPIO_Pin = GPIO_Pin_15; gpio.GPIO_Mode = GPIO_Mode_Out_PP; GPIO_Init(GPIOA, &gpio);
    /* USART2 is the fixed-rate raw soft-bus controller port. */
    gpio.GPIO_Pin = GPIO_Pin_2; gpio.GPIO_Mode = GPIO_Mode_AF_PP; GPIO_Init(GPIOA, &gpio);
    /* UART idle is high. Pull up an undriven soft-bus RX so noise cannot be
       injected onto the physical DXL bus. */
    gpio.GPIO_Pin = GPIO_Pin_3; gpio.GPIO_Mode = GPIO_Mode_IPU; GPIO_Init(GPIOA, &gpio);
    /* USART1 remains the 115200 text maintenance port. */
    gpio.GPIO_Pin = GPIO_Pin_9; gpio.GPIO_Mode = GPIO_Mode_AF_PP; GPIO_Init(GPIOA, &gpio);
    /* UART idle is high. Keep an undriven DEBUG RX from collecting noise that
       the transparent bridge would inject onto the motor bus. */
    gpio.GPIO_Pin = GPIO_Pin_10; gpio.GPIO_Mode = GPIO_Mode_IPU; GPIO_Init(GPIOA, &gpio);
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
    uart.USART_BaudRate = 115200u;
    USART_Init(USART1, &uart); USART_Cmd(USART1, ENABLE);
    uart.USART_BaudRate = SOFTBUS_BAUDRATE;
    USART_Init(USART2, &uart); USART_Cmd(USART2, ENABLE);
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
    {
        DMA_InitTypeDef dma = {0};
        NVIC_InitTypeDef nvic = {0};
        DMA_DeInit(DMA1_Channel6);
        dma.DMA_PeripheralBaseAddr = (uint32_t)&USART2->DATAR;
        dma.DMA_MemoryBaseAddr = (uint32_t)softbus_dma_rx;
        dma.DMA_DIR = DMA_DIR_PeripheralSRC;
        dma.DMA_BufferSize = IMU_POWER_SOFTBUS_MAX_BURST;
        dma.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
        dma.DMA_MemoryInc = DMA_MemoryInc_Enable;
        dma.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
        dma.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
        dma.DMA_Mode = DMA_Mode_Normal;
        dma.DMA_Priority = DMA_Priority_VeryHigh;
        dma.DMA_M2M = DMA_M2M_Disable;
        DMA_Init(DMA1_Channel6, &dma);
        imu_power_softbus_init(&softbus);
        softbus_rx_start();
        /* Clear any stale IDLE state before enabling the interrupt. */
        (void)USART2->STATR;
        (void)USART2->DATAR;
        USART_ITConfig(USART2, USART_IT_IDLE, ENABLE);
        nvic.NVIC_IRQChannel = USART2_IRQn;
        nvic.NVIC_IRQChannelPreemptionPriority = 1u;
        nvic.NVIC_IRQChannelSubPriority = 0u;
        nvic.NVIC_IRQChannelCmd = ENABLE;
        NVIC_Init(&nvic);
    }
    dxl_direction(false); spi_cs(false);
}

static bool bus_send(const uint8_t *data, size_t length, bool softbus_origin)
{
    DMA_InitTypeDef tx = {0};
    if (data == NULL || length == 0u || length > DXL_TX_SIZE) return false;
    if (softbus_origin &&
        !imu_power_softbus_begin_injection(&softbus, data, length)) return false;
    dxl_direction(true);
    DMA_DeInit(DMA1_Channel2);
    tx.DMA_PeripheralBaseAddr = (uint32_t)&USART3->DATAR;
    tx.DMA_MemoryBaseAddr = (uint32_t)data;
    tx.DMA_DIR = DMA_DIR_PeripheralDST;
    tx.DMA_BufferSize = (uint16_t)length;
    tx.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    tx.DMA_MemoryInc = DMA_MemoryInc_Enable;
    tx.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    tx.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    tx.DMA_Mode = DMA_Mode_Normal;
    tx.DMA_Priority = DMA_Priority_VeryHigh;
    tx.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(DMA1_Channel2, &tx);
    DMA_ClearFlag(DMA1_FLAG_GL2);
    /* TC is initially set. DMA writes DATAR without the status-read/data-write
       sequence that clears it, so an old TC can release the bus early. */
    USART_ClearFlag(USART3, USART_FLAG_TC);
    USART_DMACmd(USART3, USART_DMAReq_Tx, ENABLE);
    DMA_Cmd(DMA1_Channel2, ENABLE);
    while (DMA_GetFlagStatus(DMA1_FLAG_TC2) == RESET) {}
    while (USART_GetFlagStatus(USART3, USART_FLAG_TC) == RESET) {}
    DMA_Cmd(DMA1_Channel2, DISABLE);
    USART_DMACmd(USART3, USART_DMAReq_Tx, DISABLE);
    dxl_direction(false);
    return true;
}

static void handle_dxl_byte(imu_power_device_t *device, uint8_t value)
{
    imu_power_dxl_frame_t frame;
    dxl_last_rx_ms = systick_ms;
    if (imu_power_dxl_stream_feed(&dxl_rx, value, &frame,
                                  dxl_scratch, sizeof(dxl_scratch))) {
        size_t response_length = imu_power_device_handle(
            device, &frame, dxl_response, sizeof(dxl_response));
        if (response_length != 0u &&
            bus_send(dxl_response, response_length, false)) {
            /* The target board's RX path does not monitor local TX. Queue the
               exact status bytes for the USART2 soft-bus controller. */
            (void)imu_power_softbus_queue_debug(&softbus, dxl_response,
                                                response_length);
        }
    }
}

static void debug_write(const char *text)
{
    while (*text != '\0') {
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) {}
        USART_SendData(USART1, (uint8_t)*text++);
    }
    while (USART_GetFlagStatus(USART1, USART_FLAG_TC) == RESET) {}
}

static void debug_cli_poll(imu_power_device_t *device)
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

static void softbus_apply_events(imu_power_device_t *device)
{
    uint16_t events = imu_power_softbus_take_events(&softbus);
    if ((events & IMU_POWER_SOFTBUS_EVENT_TX_OVERFLOW) != 0u) {
        device->diagnostics.status_flags |= IMU_POWER_STATUS_SOFTBUS_TX_OVERFLOW;
        ++device->diagnostics.error_count;
    }
    if ((events & IMU_POWER_SOFTBUS_EVENT_ECHO_MISMATCH) != 0u) {
        device->diagnostics.status_flags |= IMU_POWER_STATUS_SOFTBUS_ECHO_MISMATCH;
        ++device->diagnostics.error_count;
    }
}

static void softbus_tx_poll(void)
{
    const uint8_t *data;
    size_t length;
    if (softbus_tx_dma_active) {
        if (DMA_GetFlagStatus(DMA1_FLAG_TC7) == RESET) return;
        DMA_Cmd(DMA1_Channel7, DISABLE);
        USART_DMACmd(USART2, USART_DMAReq_Tx, DISABLE);
        DMA_ClearFlag(DMA1_FLAG_GL7);
        imu_power_softbus_tx_consume(&softbus, softbus_tx_dma_length);
        softbus_tx_dma_active = 0u;
        softbus_tx_dma_length = 0u;
    }
    length = imu_power_softbus_tx_peek(&softbus, &data);
    if (length != 0u) {
        DMA_InitTypeDef tx = {0};
        tx.DMA_PeripheralBaseAddr = (uint32_t)&USART2->DATAR;
        tx.DMA_MemoryBaseAddr = (uint32_t)data;
        tx.DMA_DIR = DMA_DIR_PeripheralDST;
        tx.DMA_BufferSize = (uint16_t)length;
        tx.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
        tx.DMA_MemoryInc = DMA_MemoryInc_Enable;
        tx.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
        tx.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
        tx.DMA_Mode = DMA_Mode_Normal;
        tx.DMA_Priority = DMA_Priority_High;
        tx.DMA_M2M = DMA_M2M_Disable;
        DMA_DeInit(DMA1_Channel7);
        DMA_Init(DMA1_Channel7, &tx);
        DMA_ClearFlag(DMA1_FLAG_GL7);
        softbus_tx_dma_length = (uint16_t)length;
        softbus_tx_dma_active = 1u;
        USART_DMACmd(USART2, USART_DMAReq_Tx, ENABLE);
        DMA_Cmd(DMA1_Channel7, ENABLE);
    }
}

static void softbus_poll(imu_power_device_t *device)
{
    uint16_t length;
    uint8_t overflow;
    uint8_t sent = 0u;
    softbus_tx_poll();
    if (!softbus_burst_ready) return;
    length = softbus_burst_length;
    overflow = softbus_burst_overflow;
    if (overflow || length == 0u ||
        imu_power_softbus_echo_pending(&softbus) ||
        !bus_send(softbus_dma_rx, length, true)) {
        if (overflow || length != 0u) {
            device->diagnostics.status_flags |= IMU_POWER_STATUS_SOFTBUS_RX_OVERFLOW;
            ++device->diagnostics.error_count;
        }
    } else {
        uint16_t i;
        sent = 1u;
        /* Always feed the internal device directly. A physical request echo,
           when present, is suppressed from both soft-bus TX and the parser. */
        for (i = 0u; i < length; ++i) handle_dxl_byte(device, softbus_dma_rx[i]);
    }
    softbus_rx_start();
    if (sent) softbus_injection_pending_end = 1u;
    softbus_apply_events(device);
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
    imu_ready = imu_init(device) ? 1u : 0u;
    if (imu_ready) device->diagnostics.status_flags |= IMU_POWER_STATUS_IMU_READY;
    else device->diagnostics.status_flags |= IMU_POWER_STATUS_IMU_INIT_FAULT;
}

static void bus_rx_poll(imu_power_device_t *device)
{
    uint16_t dma_head = imu_power_ch32_rx_head((uint16_t)DMA1_Channel3->CNTR);
    while (dxl_rx_tail != dma_head) {
        uint8_t value = dxl_dma_rx[dxl_rx_tail];
        dxl_rx_tail = (uint16_t)((dxl_rx_tail + 1u) % DXL_RX_SIZE);
        {
            bool suppressed = false;
            (void)imu_power_softbus_observe_bus_byte(&softbus, value,
                                                     &suppressed);
            if (suppressed) continue;
        }
        handle_dxl_byte(device, value);
    }
}

void imu_power_ch32_port_poll(imu_power_device_t *device)
{
    /* Drain pre-existing bus traffic before installing a new request echo
       matcher. Then process the injected burst's echo immediately. */
    softbus_tx_poll();
    bus_rx_poll(device);
    softbus_poll(device);
    bus_rx_poll(device);
    if (softbus_injection_pending_end) {
        imu_power_softbus_end_injection(&softbus);
        softbus_injection_pending_end = 0u;
    }
    debug_cli_poll(device);
    softbus_tx_poll();
    softbus_apply_events(device);
    if (dxl_rx.length != 0u && (uint32_t)(systick_ms - dxl_last_rx_ms) > 30u) {
        dxl_rx.length = 0u;
        ++device->diagnostics.error_count;
        device->diagnostics.last_error = IMU_POWER_DXL_DATA_LENGTH;
    }
    if (imu_ready && (imu_sample_pending || systick_ms != last_sample_ms)) {
        imu_sample_pending = 0u;
        last_sample_ms = systick_ms;
        sample_update(device);
    }
    if (!imu_ready && systick_ms != last_recovery_ms && (systick_ms % 1000u) == 0u) {
        last_recovery_ms = systick_ms;
        imu_ready = imu_init(device) ? 1u : 0u;
        if (imu_ready) {
            device->diagnostics.status_flags |= IMU_POWER_STATUS_IMU_READY;
            device->diagnostics.status_flags &= (uint16_t)~IMU_POWER_STATUS_IMU_INIT_FAULT;
        } else {
            device->diagnostics.status_flags |= IMU_POWER_STATUS_IMU_INIT_FAULT;
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
