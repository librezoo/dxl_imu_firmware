#ifndef IMU_POWER_OTA_H
#define IMU_POWER_OTA_H

#include "dxl.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Flash layout (see docs/ota.md): bootloader, two application slots, two configuration pages. */
#define IMU_POWER_OTA_FLASH_BASE 0x08000000u
#define IMU_POWER_OTA_BOOT_SIZE 0x2000u
#define IMU_POWER_OTA_SLOT_COUNT 2u
#define IMU_POWER_OTA_SLOT_SIZE 0x6000u
#define IMU_POWER_OTA_PAGE_SIZE 0x1000u
#define IMU_POWER_OTA_HEADER_SIZE 0x400u /* the application starts after the header block */
#define IMU_POWER_OTA_PAYLOAD_MAX (IMU_POWER_OTA_SLOT_SIZE - IMU_POWER_OTA_HEADER_SIZE)
#define IMU_POWER_OTA_SLOT_ADDRESS(slot) \
    (IMU_POWER_OTA_FLASH_BASE + IMU_POWER_OTA_BOOT_SIZE + (uint32_t)(slot) * IMU_POWER_OTA_SLOT_SIZE)

#define IMU_POWER_OTA_HWID 0x4401u
#define IMU_POWER_OTA_BOOT_VERSION 0x0100u
/* The bootloader image ends with a 16-byte record: magic, version, hardware ID. */
#define IMU_POWER_OTA_BOOT_INFO_ADDRESS (IMU_POWER_OTA_FLASH_BASE + IMU_POWER_OTA_BOOT_SIZE - 16u)
#define IMU_POWER_OTA_BOOT_INFO_MAGIC 0x424C5844u /* "DXLB" */

/* Registers (identical in the application, which serves them read-only plus ENTER_BOOT). */
#define IMU_POWER_OTA_STATUS_ADDRESS 0x0300u
#define IMU_POWER_OTA_STATUS_LENGTH 32u
#define IMU_POWER_OTA_COMMAND_ADDRESS 0x0320u
#define IMU_POWER_OTA_BEGIN_ADDRESS 0x0328u
#define IMU_POWER_OTA_BEGIN_LENGTH 16u
#define IMU_POWER_OTA_DATA_ADDRESS 0x0400u
#define IMU_POWER_OTA_CHUNK_MAX 128u

#define IMU_POWER_OTA_CMD_UNLOCK 0xA5u
#define IMU_POWER_OTA_CMD_UNLOCK_CONFIRM 0x5Au
#define IMU_POWER_OTA_CMD_ENTER_BOOT 0x10u
#define IMU_POWER_OTA_CMD_END 0x12u
#define IMU_POWER_OTA_CMD_COMMIT 0x13u
#define IMU_POWER_OTA_CMD_ABORT 0x14u
#define IMU_POWER_OTA_CMD_REBOOT 0x15u

#define IMU_POWER_OTA_MODE_APPLICATION 1u
#define IMU_POWER_OTA_MODE_BOOTLOADER 2u

typedef enum {
    IMU_POWER_OTA_ERR_NONE = 0,
    IMU_POWER_OTA_ERR_LOCKED = 1,
    IMU_POWER_OTA_ERR_SLOT = 2,
    IMU_POWER_OTA_ERR_SIZE = 3,
    IMU_POWER_OTA_ERR_HWID = 4,
    IMU_POWER_OTA_ERR_UNSAFE = 5,   /* would leave no bootable slot */
    IMU_POWER_OTA_ERR_ORDER = 6,    /* chunk offset is not the next expected one */
    IMU_POWER_OTA_ERR_FLASH = 7,
    IMU_POWER_OTA_ERR_CRC = 8,
    IMU_POWER_OTA_ERR_STATE = 9,
    IMU_POWER_OTA_ERR_ARGS = 10,
    IMU_POWER_OTA_ERR_BOOT_VERSION = 11,
} imu_power_ota_error_t;

typedef enum {
    IMU_POWER_OTA_SLOT_EMPTY = 0,     /* no valid header */
    IMU_POWER_OTA_SLOT_VERIFIED = 1,  /* complete and checked, not yet committed */
    IMU_POWER_OTA_SLOT_TRIAL = 2,     /* committed, not yet confirmed by the running application */
    IMU_POWER_OTA_SLOT_CONFIRMED = 3,
    IMU_POWER_OTA_SLOT_BAD = 4,       /* failed three trial starts, or failed its CRC at boot */
} imu_power_ota_slot_state_t;

typedef struct {
    bool header_ok;     /* magic, version, header CRC, size, hardware ID */
    bool payload_ok;    /* payload CRC-32 (only evaluated when requested) */
    bool committed, confirmed, bad;
    uint8_t trials;
    uint16_t version;
    uint32_t seq;
    uint32_t size;
    uint32_t crc32;
    imu_power_ota_slot_state_t state;
} imu_power_ota_slot_t;

typedef struct {
    void *context;
    void (*read)(void *context, uint32_t address, void *dst, size_t size);
    /* Erase the 4 KiB page that starts at page_address. */
    bool (*erase)(void *context, uint32_t page_address);
    /* Program into erased flash; address and size are even. Image data arrives as whole, aligned
       IMU_POWER_OTA_BLOCK-byte blocks (use the fast page mode for those), state markers as half-words. */
    bool (*program)(void *context, uint32_t address, const void *src, size_t size);
} imu_power_ota_flash_t;

/* On-flash header, 64 bytes at the start of a slot (little endian):
     0 magic 'DXLI'  4 header version  6 slot  8 payload size  12 payload CRC-32  16 firmware version
    18 hardware ID  20 sequence  24 minimum bootloader version  28 CRC-32 of bytes 0..27
   followed by state half-words that are programmed one by one without erasing:
    32 committed 0xA55A  34/36/38 trial starts (programmed to 0)  40 confirmed 0x600D  42 bad 0x0BAD
   Erased flash of this chip reads 0xE339, not 0xFFFF, so a state half-word counts only when it holds
   exactly its marker value. */
#define IMU_POWER_OTA_HEADER_MAGIC 0x494C5844u
#define IMU_POWER_OTA_HEADER_VERSION 1u
#define IMU_POWER_OTA_HEADER_BYTES 64u
#define IMU_POWER_OTA_OFF_COMMITTED 32u
#define IMU_POWER_OTA_OFF_TRIAL 34u
#define IMU_POWER_OTA_OFF_CONFIRMED 40u
#define IMU_POWER_OTA_OFF_BAD 42u
#define IMU_POWER_OTA_COMMITTED 0xA55Au
#define IMU_POWER_OTA_CONFIRMED 0x600Du
#define IMU_POWER_OTA_BAD 0x0BADu
#define IMU_POWER_OTA_MAX_TRIALS 3u

/* Fills the first 32 bytes of a header (the rest is left at 0xFF); `state` bytes are not touched. */
void imu_power_ota_build_header(uint8_t header[IMU_POWER_OTA_HEADER_BYTES], unsigned slot,
                                uint32_t size, uint32_t crc32, uint16_t version,
                                uint16_t hwid, uint32_t seq);
void imu_power_ota_slot_read(const imu_power_ota_flash_t *flash, unsigned slot,
                             bool check_payload, imu_power_ota_slot_t *info);
/* Chooses the slot to start (-1 if none) and records a trial start or a failed trial. */
int imu_power_ota_select(const imu_power_ota_flash_t *flash);
/* Marks the running slot confirmed. */
bool imu_power_ota_confirm(const imu_power_ota_flash_t *flash, unsigned slot);

/* Image data is programmed in blocks of this size (the flash "fast page" of the chip). */
#define IMU_POWER_OTA_BLOCK 256u

/* Update session, run by the bootloader (and by the host tests). */
typedef struct {
    imu_power_ota_flash_t flash;
    uint8_t id;
    uint8_t unlock_step;
    bool unlocked;
    uint8_t state;      /* 0 idle, 1 receiving, 2 verified */
    uint8_t slot;
    uint8_t last_error;
    uint8_t erased_pages; /* bit per page of the target slot */
    uint16_t version;
    uint32_t size, crc32, next, seq;
    uint16_t fill;      /* bytes waiting in `block`; next - fill bytes are already programmed */
    bool reboot_requested;
    union { uint32_t word[IMU_POWER_OTA_BLOCK / 4u]; uint8_t byte[IMU_POWER_OTA_BLOCK]; } block;
} imu_power_ota_t;

void imu_power_ota_init(imu_power_ota_t *ota, const imu_power_ota_flash_t *flash, uint8_t id);
size_t imu_power_ota_handle(imu_power_ota_t *ota, const imu_power_dxl_frame_t *frame,
                            uint8_t *response, size_t capacity);

/* The 32-byte status block, shared by the bootloader and the application. */
void imu_power_ota_status_block(uint8_t out[IMU_POWER_OTA_STATUS_LENGTH], unsigned mode,
                                uint16_t firmware_version, uint16_t boot_version,
                                unsigned running_slot, const imu_power_ota_slot_t slots[2],
                                unsigned session_state, uint32_t next, unsigned last_error);

#endif
