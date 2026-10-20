#ifndef IMU_POWER_DXL_H
#define IMU_POWER_DXL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IMU_POWER_DXL_BROADCAST 0xFEu
#define IMU_POWER_DXL_MAX_PACKET 512u

typedef enum {
    IMU_POWER_DXL_PING = 0x01,
    IMU_POWER_DXL_READ = 0x02,
    IMU_POWER_DXL_WRITE = 0x03,
    IMU_POWER_DXL_REG_WRITE = 0x04,
    IMU_POWER_DXL_ACTION = 0x05,
    IMU_POWER_DXL_RESET = 0x06,
    IMU_POWER_DXL_STATUS = 0x55,
    IMU_POWER_DXL_SYNC_READ = 0x82,
    IMU_POWER_DXL_SYNC_WRITE = 0x83,
    IMU_POWER_DXL_FAST_SYNC_READ = 0x8A,
} imu_power_dxl_instruction_t;

typedef struct {
    uint8_t id;
    uint8_t instruction;
    const uint8_t *parameters;
    size_t parameter_length;
    bool broadcast;
} imu_power_dxl_frame_t;

typedef struct {
    uint8_t packet[IMU_POWER_DXL_MAX_PACKET];
    size_t length;
} imu_power_dxl_stream_t;

typedef enum {
    IMU_POWER_DXL_OK = 0,
    IMU_POWER_DXL_RESULT_FAIL = 1,
    IMU_POWER_DXL_INSTRUCTION = 2,
    IMU_POWER_DXL_CRC = 3,
    IMU_POWER_DXL_DATA_RANGE = 4,
    IMU_POWER_DXL_DATA_LENGTH = 5,
    IMU_POWER_DXL_ACCESS = 6,
} imu_power_dxl_error_t;

uint16_t imu_power_dxl_crc16(const uint8_t *data, size_t length);
bool imu_power_dxl_parse(const uint8_t *packet, size_t packet_length,
                         imu_power_dxl_frame_t *frame,
                         uint8_t *scratch, size_t scratch_size);
size_t imu_power_dxl_build_status(uint8_t id, uint8_t error,
                                  const uint8_t *payload, size_t payload_length,
                                  uint8_t *out, size_t out_capacity);

/* The first block of the answer to a Fast Sync Read (0x8A). All devices of the request answer with ONE status
   packet from the broadcast ID: the first device sends the header and its block, each following device appends
   its own block after the one before it. Layout:

       FF FF FD 00 FE LEN_L LEN_H 55 | ERR ID DATA(n) CRC_L CRC_H | ERR ID DATA(n) CRC_L CRC_H | ...

   LEN = 1 + total_blocks * (n + 4), counting every device of the request, not only this one. Each CRC covers the
   whole packet from the first header byte up to and including that block's data, so the last one is the CRC of
   the packet. Nothing is byte-stuffed. This builds the header and the block of the FIRST device (n + 12 bytes);
   returns 0 when it does not fit, when total_blocks is 0 or when LEN would not fit in 16 bits. */
size_t imu_power_dxl_build_fast_sync_first_block(uint16_t total_blocks, uint8_t id, uint8_t error,
                                                 const uint8_t *data, size_t data_length,
                                                 uint8_t *out, size_t out_capacity);

/* Feed wire bytes, consuming complete bad frames too. A new unescaped header
   resynchronizes a truncated frame. The returned frame points into scratch. */
bool imu_power_dxl_stream_feed(imu_power_dxl_stream_t *stream, uint8_t byte,
                               imu_power_dxl_frame_t *frame,
                               uint8_t *scratch, size_t scratch_size);

#endif
