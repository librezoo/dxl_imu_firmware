#include "imu_power/ota.h"

#include "imu_power/config.h"

#include <string.h>

static uint16_t get_u16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8u);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) | ((uint32_t)p[3] << 24u);
}

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8u);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8u); p[2] = (uint8_t)(v >> 16u); p[3] = (uint8_t)(v >> 24u);
}

void imu_power_ota_build_header(uint8_t header[IMU_POWER_OTA_HEADER_BYTES], unsigned slot,
                                uint32_t size, uint32_t crc32, uint16_t version,
                                uint16_t hwid, uint32_t seq)
{
    memset(header, 0xFF, IMU_POWER_OTA_HEADER_BYTES);
    put_u32(&header[0], IMU_POWER_OTA_HEADER_MAGIC);
    put_u16(&header[4], IMU_POWER_OTA_HEADER_VERSION);
    put_u16(&header[6], (uint16_t)slot);
    put_u32(&header[8], size);
    put_u32(&header[12], crc32);
    put_u16(&header[16], version);
    put_u16(&header[18], hwid);
    put_u32(&header[20], seq);
    put_u16(&header[24], IMU_POWER_OTA_BOOT_VERSION);
    put_u16(&header[26], 0u);
    put_u32(&header[28], imu_power_crc32(header, 28u));
}

static uint32_t payload_crc(const imu_power_ota_flash_t *flash, unsigned slot, uint32_t size)
{
    uint8_t block[64];
    uint32_t state = 0xFFFFFFFFu, address = IMU_POWER_OTA_SLOT_ADDRESS(slot) + IMU_POWER_OTA_HEADER_SIZE;
    while (size != 0u) {
        uint32_t n = size < sizeof(block) ? size : (uint32_t)sizeof(block);
        flash->read(flash->context, address, block, n);
        state = imu_power_crc32_update(state, block, n);
        address += n; size -= n;
    }
    return ~state;
}

void imu_power_ota_slot_read(const imu_power_ota_flash_t *flash, unsigned slot,
                             bool check_payload, imu_power_ota_slot_t *info)
{
    uint8_t h[IMU_POWER_OTA_HEADER_BYTES];
    unsigned i;
    memset(info, 0, sizeof(*info));
    flash->read(flash->context, IMU_POWER_OTA_SLOT_ADDRESS(slot), h, sizeof(h));
    if (get_u32(&h[0]) != IMU_POWER_OTA_HEADER_MAGIC || get_u16(&h[4]) != IMU_POWER_OTA_HEADER_VERSION ||
        get_u16(&h[6]) != slot || get_u16(&h[18]) != IMU_POWER_OTA_HWID ||
        get_u16(&h[24]) > IMU_POWER_OTA_BOOT_VERSION ||
        get_u32(&h[28]) != imu_power_crc32(h, 28u) ||
        get_u32(&h[8]) == 0u || get_u32(&h[8]) > IMU_POWER_OTA_PAYLOAD_MAX) {
        return;
    }
    info->header_ok = true;
    info->size = get_u32(&h[8]);
    info->crc32 = get_u32(&h[12]);
    info->version = get_u16(&h[16]);
    info->seq = get_u32(&h[20]);
    info->committed = get_u16(&h[IMU_POWER_OTA_OFF_COMMITTED]) == IMU_POWER_OTA_COMMITTED;
    info->confirmed = get_u16(&h[IMU_POWER_OTA_OFF_CONFIRMED]) == IMU_POWER_OTA_CONFIRMED;
    info->bad = get_u16(&h[IMU_POWER_OTA_OFF_BAD]) == IMU_POWER_OTA_BAD;
    for (i = 0; i < IMU_POWER_OTA_MAX_TRIALS; ++i) {
        if (get_u16(&h[IMU_POWER_OTA_OFF_TRIAL + 2u * i]) == 0u) ++info->trials;
    }
    if (check_payload) info->payload_ok = payload_crc(flash, slot, info->size) == info->crc32;
    if (info->bad) info->state = IMU_POWER_OTA_SLOT_BAD;
    else if (info->confirmed) info->state = IMU_POWER_OTA_SLOT_CONFIRMED;
    else if (info->committed) info->state = IMU_POWER_OTA_SLOT_TRIAL;
    else info->state = IMU_POWER_OTA_SLOT_VERIFIED;
}

static bool program_u16(const imu_power_ota_flash_t *flash, unsigned slot, unsigned offset, uint16_t value)
{
    uint8_t b[2];
    put_u16(b, value);
    return flash->program(flash->context, IMU_POWER_OTA_SLOT_ADDRESS(slot) + offset, b, 2u);
}

int imu_power_ota_select(const imu_power_ota_flash_t *flash)
{
    imu_power_ota_slot_t info[IMU_POWER_OTA_SLOT_COUNT];
    int trial = -1, confirmed = -1;
    unsigned s;
    for (s = 0; s < IMU_POWER_OTA_SLOT_COUNT; ++s) {
        imu_power_ota_slot_read(flash, s, true, &info[s]);
        if (!info[s].header_ok || info[s].bad || info[s].state == IMU_POWER_OTA_SLOT_VERIFIED) continue;
        if (!info[s].payload_ok) { /* damaged since it was verified: never start it */
            (void)program_u16(flash, s, IMU_POWER_OTA_OFF_BAD, IMU_POWER_OTA_BAD);
            continue;
        }
        if (info[s].confirmed) {
            if (confirmed < 0 || info[s].seq > info[confirmed].seq) confirmed = (int)s;
        } else if (info[s].trials >= IMU_POWER_OTA_MAX_TRIALS) {
            (void)program_u16(flash, s, IMU_POWER_OTA_OFF_BAD, IMU_POWER_OTA_BAD);
        } else if (trial < 0 || info[s].seq > info[trial].seq) {
            trial = (int)s;
        }
    }
    if (trial >= 0) {
        (void)program_u16(flash, (unsigned)trial,
                          IMU_POWER_OTA_OFF_TRIAL + 2u * info[trial].trials, 0u);
        return trial;
    }
    return confirmed;
}

bool imu_power_ota_confirm(const imu_power_ota_flash_t *flash, unsigned slot)
{
    imu_power_ota_slot_t info;
    imu_power_ota_slot_read(flash, slot, false, &info);
    if (!info.header_ok || !info.committed || info.bad) return false;
    if (info.confirmed) return true;
    return program_u16(flash, slot, IMU_POWER_OTA_OFF_CONFIRMED, IMU_POWER_OTA_CONFIRMED);
}

void imu_power_ota_status_block(uint8_t out[IMU_POWER_OTA_STATUS_LENGTH], unsigned mode,
                                uint16_t firmware_version, uint16_t boot_version,
                                unsigned running_slot, const imu_power_ota_slot_t slots[2],
                                unsigned session_state, uint32_t next, unsigned last_error)
{
    memset(out, 0, IMU_POWER_OTA_STATUS_LENGTH);
    put_u16(&out[0], (uint16_t)mode);
    put_u16(&out[2], firmware_version);
    put_u16(&out[4], boot_version);
    out[6] = (uint8_t)running_slot;
    out[7] = (uint8_t)slots[0].state;
    out[8] = (uint8_t)slots[1].state;
    out[9] = (uint8_t)last_error;
    put_u16(&out[10], IMU_POWER_OTA_HWID);
    put_u16(&out[12], IMU_POWER_OTA_CHUNK_MAX);
    out[14] = (uint8_t)session_state;
    put_u32(&out[16], next);
    put_u16(&out[20], slots[0].version);
    put_u16(&out[22], slots[1].version);
    put_u32(&out[24], slots[0].seq);
    put_u32(&out[28], slots[1].seq);
}

void imu_power_ota_init(imu_power_ota_t *ota, const imu_power_ota_flash_t *flash, uint8_t id)
{
    memset(ota, 0, sizeof(*ota));
    ota->flash = *flash;
    ota->id = id;
}

/* A slot can be started if it was committed and has not been rejected. */
static bool usable(const imu_power_ota_slot_t *s)
{
    return s->header_ok && s->payload_ok && !s->bad && s->committed;
}

static uint8_t fail(imu_power_ota_t *ota, imu_power_ota_error_t error, uint8_t dxl_error)
{
    ota->last_error = (uint8_t)error;
    return dxl_error;
}

static uint8_t begin(imu_power_ota_t *ota, const uint8_t *a)
{
    imu_power_ota_slot_t info[IMU_POWER_OTA_SLOT_COUNT];
    unsigned slot = a[0], other;
    uint32_t size = get_u32(&a[4]);
    if (!ota->unlocked) return fail(ota, IMU_POWER_OTA_ERR_LOCKED, IMU_POWER_DXL_ACCESS);
    if (slot >= IMU_POWER_OTA_SLOT_COUNT || a[1] != 0u) return fail(ota, IMU_POWER_OTA_ERR_SLOT, IMU_POWER_DXL_RESULT_FAIL);
    if (size == 0u || size > IMU_POWER_OTA_PAYLOAD_MAX) return fail(ota, IMU_POWER_OTA_ERR_SIZE, IMU_POWER_DXL_RESULT_FAIL);
    if (get_u16(&a[12]) != IMU_POWER_OTA_HWID) return fail(ota, IMU_POWER_OTA_ERR_HWID, IMU_POWER_DXL_RESULT_FAIL);
    if (get_u16(&a[14]) > IMU_POWER_OTA_BOOT_VERSION) return fail(ota, IMU_POWER_OTA_ERR_BOOT_VERSION, IMU_POWER_DXL_RESULT_FAIL);
    other = slot ^ 1u;
    imu_power_ota_slot_read(&ota->flash, slot, true, &info[0]);
    imu_power_ota_slot_read(&ota->flash, other, true, &info[1]);
    /* Never destroy the only slot that can be started. */
    if (usable(&info[0]) && !usable(&info[1])) return fail(ota, IMU_POWER_OTA_ERR_UNSAFE, IMU_POWER_DXL_RESULT_FAIL);
    ota->seq = (info[1].header_ok ? info[1].seq : 0u) + 1u;
    ota->state = 0u;
    if (!ota->flash.erase(ota->flash.context, IMU_POWER_OTA_SLOT_ADDRESS(slot)))
        return fail(ota, IMU_POWER_OTA_ERR_FLASH, IMU_POWER_DXL_RESULT_FAIL);
    ota->slot = (uint8_t)slot;
    ota->size = size;
    ota->crc32 = get_u32(&a[8]);
    ota->version = get_u16(&a[2]);
    ota->next = 0u;
    ota->fill = 0u;
    ota->erased_pages = 1u;
    ota->state = 1u;
    ota->last_error = IMU_POWER_OTA_ERR_NONE;
    return IMU_POWER_DXL_OK;
}

/* Programs the block buffer (padded with 0xFF) at the next free block of the slot. */
static uint8_t flush_block(imu_power_ota_t *ota)
{
    uint32_t offset = ota->next - ota->fill; /* payload offset of the block, a multiple of the block size */
    uint32_t address = IMU_POWER_OTA_HEADER_SIZE + offset;
    uint32_t page = address / IMU_POWER_OTA_PAGE_SIZE;
    if ((ota->erased_pages & (1u << page)) == 0u) {
        if (!ota->flash.erase(ota->flash.context, IMU_POWER_OTA_SLOT_ADDRESS(ota->slot) + page * IMU_POWER_OTA_PAGE_SIZE))
            return fail(ota, IMU_POWER_OTA_ERR_FLASH, IMU_POWER_DXL_RESULT_FAIL);
        ota->erased_pages |= (uint8_t)(1u << page);
    }
    memset(&ota->block.byte[ota->fill], 0xFF, IMU_POWER_OTA_BLOCK - ota->fill);
    if (!ota->flash.program(ota->flash.context, IMU_POWER_OTA_SLOT_ADDRESS(ota->slot) + address,
                            ota->block.byte, IMU_POWER_OTA_BLOCK))
        return fail(ota, IMU_POWER_OTA_ERR_FLASH, IMU_POWER_DXL_RESULT_FAIL);
    ota->fill = 0u;
    return IMU_POWER_DXL_OK;
}

static uint8_t chunk(imu_power_ota_t *ota, const uint8_t *data, size_t length)
{
    uint32_t offset, n, i;
    if (!ota->unlocked) return fail(ota, IMU_POWER_OTA_ERR_LOCKED, IMU_POWER_DXL_ACCESS);
    if (ota->state != 1u) return fail(ota, IMU_POWER_OTA_ERR_STATE, IMU_POWER_DXL_RESULT_FAIL);
    if (length < 5u || length - 4u > IMU_POWER_OTA_CHUNK_MAX) return fail(ota, IMU_POWER_OTA_ERR_ARGS, IMU_POWER_DXL_DATA_LENGTH);
    offset = get_u32(data);
    n = (uint32_t)(length - 4u);
    data += 4;
    if (offset + n > ota->size || offset + n < offset) return fail(ota, IMU_POWER_OTA_ERR_SIZE, IMU_POWER_DXL_DATA_RANGE);
    if (offset + n <= ota->next) return IMU_POWER_DXL_OK; /* repeated chunk whose answer was lost */
    if (offset != ota->next) return fail(ota, IMU_POWER_OTA_ERR_ORDER, IMU_POWER_DXL_DATA_RANGE);
    for (i = 0; i < n; ++i) {
        ota->block.byte[ota->fill++] = data[i];
        ++ota->next;
        if (ota->fill == IMU_POWER_OTA_BLOCK) {
            uint8_t error = flush_block(ota);
            if (error != IMU_POWER_DXL_OK) return error;
        }
    }
    return IMU_POWER_DXL_OK;
}

static uint8_t finish(imu_power_ota_t *ota)
{
    uint8_t header[IMU_POWER_OTA_HEADER_BYTES];
    if (!ota->unlocked) return fail(ota, IMU_POWER_OTA_ERR_LOCKED, IMU_POWER_DXL_ACCESS);
    if (ota->state == 2u) return IMU_POWER_DXL_OK; /* repeated END whose answer was lost */
    if (ota->state != 1u || ota->next != ota->size) return fail(ota, IMU_POWER_OTA_ERR_STATE, IMU_POWER_DXL_RESULT_FAIL);
    if (ota->fill != 0u) {
        uint8_t error = flush_block(ota);
        if (error != IMU_POWER_DXL_OK) { ota->state = 0u; return error; }
    }
    if (payload_crc(&ota->flash, ota->slot, ota->size) != ota->crc32) {
        ota->state = 0u;
        return fail(ota, IMU_POWER_OTA_ERR_CRC, IMU_POWER_DXL_RESULT_FAIL);
    }
    imu_power_ota_build_header(header, ota->slot, ota->size, ota->crc32, ota->version,
                               IMU_POWER_OTA_HWID, ota->seq);
    /* Only the CRC-protected part; the state half-words stay erased. The header CRC comes last, so an
       interrupted write never forms a valid header. */
    if (!ota->flash.program(ota->flash.context, IMU_POWER_OTA_SLOT_ADDRESS(ota->slot), header, 32u)) {
        ota->state = 0u;
        return fail(ota, IMU_POWER_OTA_ERR_FLASH, IMU_POWER_DXL_RESULT_FAIL);
    }
    ota->state = 2u;
    return IMU_POWER_DXL_OK;
}

static uint8_t command(imu_power_ota_t *ota, uint8_t cmd)
{
    if (cmd == IMU_POWER_OTA_CMD_UNLOCK) {
        ota->unlock_step = 1u;
        return IMU_POWER_DXL_OK;
    }
    if (cmd == IMU_POWER_OTA_CMD_UNLOCK_CONFIRM && ota->unlock_step == 1u) {
        ota->unlocked = true; ota->unlock_step = 0u;
        return IMU_POWER_DXL_OK;
    }
    ota->unlock_step = 0u;
    if (!ota->unlocked) return fail(ota, IMU_POWER_OTA_ERR_LOCKED, IMU_POWER_DXL_ACCESS);
    switch (cmd) {
    case IMU_POWER_OTA_CMD_END:
        return finish(ota);
    case IMU_POWER_OTA_CMD_COMMIT:
        if (ota->state != 2u) return fail(ota, IMU_POWER_OTA_ERR_STATE, IMU_POWER_DXL_RESULT_FAIL);
        if (!program_u16(&ota->flash, ota->slot, IMU_POWER_OTA_OFF_COMMITTED, IMU_POWER_OTA_COMMITTED))
            return fail(ota, IMU_POWER_OTA_ERR_FLASH, IMU_POWER_DXL_RESULT_FAIL);
        ota->state = 0u;
        ota->reboot_requested = true;
        return IMU_POWER_DXL_OK;
    case IMU_POWER_OTA_CMD_ABORT:
        ota->state = 0u;
        return IMU_POWER_DXL_OK;
    case IMU_POWER_OTA_CMD_REBOOT:
        ota->reboot_requested = true;
        return IMU_POWER_DXL_OK;
    default:
        return fail(ota, IMU_POWER_OTA_ERR_ARGS, IMU_POWER_DXL_DATA_RANGE);
    }
}

size_t imu_power_ota_handle(imu_power_ota_t *ota, const imu_power_dxl_frame_t *frame,
                            uint8_t *response, size_t capacity)
{
    uint16_t address, length;
    uint8_t error;
    if (frame->instruction == IMU_POWER_DXL_STATUS || frame->broadcast || frame->id != ota->id) return 0u;
    switch (frame->instruction) {
    case IMU_POWER_DXL_PING:
        return imu_power_dxl_build_status(ota->id, 0u, NULL, 0u, response, capacity);
    case IMU_POWER_DXL_READ:
        if (frame->parameter_length != 4u) return 0u;
        address = get_u16(frame->parameters);
        length = get_u16(&frame->parameters[2]);
        if (address == IMU_POWER_OTA_STATUS_ADDRESS && length != 0u && length <= IMU_POWER_OTA_STATUS_LENGTH) {
            imu_power_ota_slot_t slots[2];
            uint8_t block[IMU_POWER_OTA_STATUS_LENGTH];
            imu_power_ota_slot_read(&ota->flash, 0u, false, &slots[0]);
            imu_power_ota_slot_read(&ota->flash, 1u, false, &slots[1]);
            imu_power_ota_status_block(block, IMU_POWER_OTA_MODE_BOOTLOADER, 0u, IMU_POWER_OTA_BOOT_VERSION,
                                       0xFFu, slots, ota->state, ota->next, ota->last_error);
            return imu_power_dxl_build_status(ota->id, 0u, block, length, response, capacity);
        }
        return imu_power_dxl_build_status(ota->id, IMU_POWER_DXL_DATA_RANGE, NULL, 0u, response, capacity);
    case IMU_POWER_DXL_WRITE:
        if (frame->parameter_length < 3u) return 0u;
        address = get_u16(frame->parameters);
        length = (uint16_t)(frame->parameter_length - 2u);
        if (address == IMU_POWER_OTA_COMMAND_ADDRESS && length == 1u) error = command(ota, frame->parameters[2]);
        else if (address == IMU_POWER_OTA_BEGIN_ADDRESS && length == IMU_POWER_OTA_BEGIN_LENGTH) error = begin(ota, &frame->parameters[2]);
        else if (address == IMU_POWER_OTA_DATA_ADDRESS) error = chunk(ota, &frame->parameters[2], length);
        else error = IMU_POWER_DXL_ACCESS;
        return imu_power_dxl_build_status(ota->id, error, NULL, 0u, response, capacity);
    case IMU_POWER_DXL_RESET:
        ota->reboot_requested = true;
        return imu_power_dxl_build_status(ota->id, 0u, NULL, 0u, response, capacity);
    default:
        return imu_power_dxl_build_status(ota->id, IMU_POWER_DXL_INSTRUCTION, NULL, 0u, response, capacity);
    }
}
