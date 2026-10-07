#include "imu_power/config.h"
#include "imu_power/ota.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Simulated 64 KiB flash. A budget of operations (page erase, half-word program) lets a test cut the power
   at any point; an interrupted erase leaves garbage in the page, an interrupted program stops after the
   half-words already written. As on the chip, an erased half-word reads 0xE339 and only an erased (or
   0xFFFF-programmed) half-word can be programmed. */
#define ERASED_LO 0x39u
#define ERASED_HI 0xE3u
static uint8_t mem[0x10000];
static long budget = -1;
static long ops_done;

static void sim_read(void *ctx, uint32_t address, void *dst, size_t size)
{
    (void)ctx;
    assert(address >= IMU_POWER_OTA_FLASH_BASE && address - IMU_POWER_OTA_FLASH_BASE + size <= sizeof(mem));
    memcpy(dst, &mem[address - IMU_POWER_OTA_FLASH_BASE], size);
}

static bool spend(void)
{
    if (budget == 0) return false;
    if (budget > 0) --budget;
    ++ops_done;
    return true;
}

static bool sim_erase(void *ctx, uint32_t page)
{
    uint8_t *p = &mem[page - IMU_POWER_OTA_FLASH_BASE];
    size_t i;
    (void)ctx;
    assert(page % IMU_POWER_OTA_PAGE_SIZE == 0u && page >= IMU_POWER_OTA_FLASH_BASE + IMU_POWER_OTA_BOOT_SIZE);
    if (!spend()) { memset(p, 0x5A, IMU_POWER_OTA_PAGE_SIZE); return false; }
    for (i = 0; i < IMU_POWER_OTA_PAGE_SIZE; i += 2u) { p[i] = ERASED_LO; p[i + 1u] = ERASED_HI; }
    return true;
}

static bool sim_program(void *ctx, uint32_t address, const void *src, size_t size)
{
    size_t i;
    (void)ctx;
    assert((address & 1u) == 0u && (size & 1u) == 0u);
    assert(address >= IMU_POWER_OTA_FLASH_BASE + IMU_POWER_OTA_BOOT_SIZE);
    for (i = 0; i < size; i += 2u) {
        uint8_t *p = &mem[address - IMU_POWER_OTA_FLASH_BASE + i];
        if (!spend()) return false;
        if (!((p[0] == ERASED_LO && p[1] == ERASED_HI) || (p[0] == 0xFFu && p[1] == 0xFFu))) return false;
        memcpy(p, (const uint8_t *)src + i, 2u);
    }
    return true;
}

static const imu_power_ota_flash_t flash = { NULL, sim_read, sim_erase, sim_program };

typedef struct { uint8_t data[0x3000]; uint32_t size; uint32_t crc; uint16_t version; } image_t;

static void make_image(image_t *img, uint8_t seed, uint32_t size, uint16_t version)
{
    uint32_t i;
    for (i = 0; i < size; ++i) img->data[i] = (uint8_t)(seed + i * 7u + (i >> 8u));
    img->size = size; img->version = version; img->crc = imu_power_crc32(img->data, size);
}

/* Programs an image the way the production tool does: payload, then a confirmed header. */
static void install(unsigned slot, const image_t *img, uint32_t seq)
{
    uint8_t h[IMU_POWER_OTA_HEADER_BYTES];
    uint32_t base = IMU_POWER_OTA_SLOT_ADDRESS(slot) - IMU_POWER_OTA_FLASH_BASE;
    memset(&mem[base], 0xFF, IMU_POWER_OTA_SLOT_SIZE);
    memcpy(&mem[base + IMU_POWER_OTA_HEADER_SIZE], img->data, img->size);
    imu_power_ota_build_header(h, slot, img->size, img->crc, img->version, IMU_POWER_OTA_HWID, seq);
    h[IMU_POWER_OTA_OFF_CONFIRMED] = (uint8_t)IMU_POWER_OTA_CONFIRMED;
    h[IMU_POWER_OTA_OFF_CONFIRMED + 1] = (uint8_t)(IMU_POWER_OTA_CONFIRMED >> 8);
    h[IMU_POWER_OTA_OFF_COMMITTED] = (uint8_t)IMU_POWER_OTA_COMMITTED;
    h[IMU_POWER_OTA_OFF_COMMITTED + 1] = (uint8_t)(IMU_POWER_OTA_COMMITTED >> 8);
    memcpy(&mem[base], h, sizeof(h));
}

static void erase_all(void)
{
    size_t i;
    for (i = 0; i < sizeof(mem); i += 2u) { mem[i] = ERASED_LO; mem[i + 1u] = ERASED_HI; }
    budget = -1;
}

static uint8_t send(imu_power_ota_t *o, uint8_t instruction, const uint8_t *params, size_t length)
{
    imu_power_dxl_frame_t f = { o->id, instruction, params, length, false };
    uint8_t out[600];
    size_t n = imu_power_ota_handle(o, &f, out, sizeof(out));
    if (n == 0u) return 0xFFu;
    return out[8];
}

static uint8_t write_reg(imu_power_ota_t *o, uint16_t address, const uint8_t *data, size_t length)
{
    uint8_t p[2 + 4 + IMU_POWER_OTA_CHUNK_MAX];
    p[0] = (uint8_t)address; p[1] = (uint8_t)(address >> 8);
    memcpy(&p[2], data, length);
    return send(o, IMU_POWER_DXL_WRITE, p, 2u + length);
}

static uint8_t cmd(imu_power_ota_t *o, uint8_t c) { return write_reg(o, IMU_POWER_OTA_COMMAND_ADDRESS, &c, 1u); }

static uint8_t unlock(imu_power_ota_t *o)
{
    uint8_t r = cmd(o, IMU_POWER_OTA_CMD_UNLOCK);
    return r != 0u ? r : cmd(o, IMU_POWER_OTA_CMD_UNLOCK_CONFIRM);
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint8_t begin(imu_power_ota_t *o, unsigned slot, const image_t *img, uint16_t hwid, uint32_t size)
{
    uint8_t a[16] = {0};
    a[0] = (uint8_t)slot; a[2] = (uint8_t)img->version; a[3] = (uint8_t)(img->version >> 8);
    put32(&a[4], size); put32(&a[8], img->crc); a[12] = (uint8_t)hwid; a[13] = (uint8_t)(hwid >> 8);
    a[14] = (uint8_t)IMU_POWER_OTA_BOOT_VERSION; a[15] = (uint8_t)(IMU_POWER_OTA_BOOT_VERSION >> 8);
    return write_reg(o, IMU_POWER_OTA_BEGIN_ADDRESS, a, sizeof(a));
}

static uint8_t chunk(imu_power_ota_t *o, const image_t *img, uint32_t offset, uint32_t n)
{
    uint8_t d[4 + IMU_POWER_OTA_CHUNK_MAX];
    put32(d, offset);
    memcpy(&d[4], &img->data[offset], n);
    return write_reg(o, IMU_POWER_OTA_DATA_ADDRESS, d, 4u + n);
}

/* Runs a whole update; returns the first non-zero DXL error (0 when everything succeeded). */
static uint8_t update(imu_power_ota_t *o, unsigned slot, const image_t *img, bool commit)
{
    uint32_t off;
    uint8_t r;
    if ((r = unlock(o)) != 0u) return r;
    if ((r = begin(o, slot, img, IMU_POWER_OTA_HWID, img->size)) != 0u) return r;
    for (off = 0; off < img->size; off += IMU_POWER_OTA_CHUNK_MAX) {
        uint32_t n = img->size - off < IMU_POWER_OTA_CHUNK_MAX ? img->size - off : IMU_POWER_OTA_CHUNK_MAX;
        if ((r = chunk(o, img, off, n)) != 0u) return r;
    }
    if ((r = cmd(o, IMU_POWER_OTA_CMD_END)) != 0u) return r;
    return commit ? cmd(o, IMU_POWER_OTA_CMD_COMMIT) : 0u;
}

static imu_power_ota_t session(void)
{
    imu_power_ota_t o;
    imu_power_ota_init(&o, &flash, 200u);
    return o;
}

static bool slot_holds(unsigned slot, const image_t *img)
{
    return memcmp(&mem[IMU_POWER_OTA_SLOT_ADDRESS(slot) - IMU_POWER_OTA_FLASH_BASE + IMU_POWER_OTA_HEADER_SIZE],
                  img->data, img->size) == 0;
}

static image_t imgA, imgB, imgC;

/* True once the first image block of a slot has been written (no longer reads as erased). */
static bool slot_block_programmed(unsigned slot, uint32_t offset)
{
    return mem[IMU_POWER_OTA_SLOT_ADDRESS(slot) - IMU_POWER_OTA_FLASH_BASE + IMU_POWER_OTA_HEADER_SIZE + offset] ==
           imgB.data[offset];
}

static void test_select_and_trials(void)
{
    imu_power_ota_t o;
    imu_power_ota_slot_t info;
    erase_all();
    assert(imu_power_ota_select(&flash) == -1);
    install(0, &imgA, 1u);
    assert(imu_power_ota_select(&flash) == 0);

    o = session();
    assert(update(&o, 1u, &imgB, false) == 0u);
    imu_power_ota_slot_read(&flash, 1u, true, &info);
    assert(info.state == IMU_POWER_OTA_SLOT_VERIFIED && info.seq == 2u && info.version == imgB.version);
    assert(cmd(&o, IMU_POWER_OTA_CMD_END) == 0u); /* repeated END is harmless */
    assert(imu_power_ota_select(&flash) == 0); /* verified but not committed: not started */
    assert(cmd(&o, IMU_POWER_OTA_CMD_COMMIT) == 0u && o.reboot_requested);
    assert(imu_power_ota_select(&flash) == 1);
    imu_power_ota_slot_read(&flash, 1u, false, &info);
    assert(info.state == IMU_POWER_OTA_SLOT_TRIAL && info.trials == 1u);
    assert(imu_power_ota_select(&flash) == 1);
    assert(imu_power_ota_select(&flash) == 1);
    assert(imu_power_ota_select(&flash) == 0); /* three unconfirmed starts: fall back */
    imu_power_ota_slot_read(&flash, 1u, false, &info);
    assert(info.state == IMU_POWER_OTA_SLOT_BAD);
    assert(imu_power_ota_select(&flash) == 0);

    /* A confirmed update stays selected without further trial starts. */
    o = session();
    assert(update(&o, 1u, &imgC, true) == 0u);
    assert(imu_power_ota_select(&flash) == 1);
    assert(imu_power_ota_confirm(&flash, 1u));
    assert(imu_power_ota_select(&flash) == 1);
    imu_power_ota_slot_read(&flash, 1u, false, &info);
    assert(info.state == IMU_POWER_OTA_SLOT_CONFIRMED && info.trials == 1u && info.seq == 2u);

    /* Updating the other slot next: the old one stays until the new one is committed. */
    o = session();
    assert(update(&o, 0u, &imgA, false) == 0u);
    assert(imu_power_ota_select(&flash) == 1);
    assert(cmd(&o, IMU_POWER_OTA_CMD_COMMIT) == 0u);
    assert(imu_power_ota_select(&flash) == 0);
}

static void test_rejections(void)
{
    imu_power_ota_t o;
    image_t bad = imgB;
    erase_all();
    install(0, &imgA, 1u);
    o = session();
    assert(begin(&o, 1u, &imgB, IMU_POWER_OTA_HWID, imgB.size) == IMU_POWER_DXL_ACCESS); /* locked */
    assert(cmd(&o, IMU_POWER_OTA_CMD_END) == IMU_POWER_DXL_ACCESS);
    assert(cmd(&o, IMU_POWER_OTA_CMD_UNLOCK_CONFIRM) == IMU_POWER_DXL_ACCESS); /* skipped step one */
    assert(unlock(&o) == 0u);
    assert(begin(&o, 2u, &imgB, IMU_POWER_OTA_HWID, imgB.size) != 0u && o.last_error == IMU_POWER_OTA_ERR_SLOT);
    assert(begin(&o, 1u, &imgB, IMU_POWER_OTA_HWID, IMU_POWER_OTA_PAYLOAD_MAX + 1u) != 0u &&
           o.last_error == IMU_POWER_OTA_ERR_SIZE);
    assert(begin(&o, 1u, &imgB, 0x1234u, imgB.size) != 0u && o.last_error == IMU_POWER_OTA_ERR_HWID);
    /* Slot 0 is the only startable slot: it must not be erased. */
    assert(begin(&o, 0u, &imgB, IMU_POWER_OTA_HWID, imgB.size) != 0u && o.last_error == IMU_POWER_OTA_ERR_UNSAFE);
    assert(slot_holds(0, &imgA));
    assert(chunk(&o, &imgB, 0u, 16u) != 0u && o.last_error == IMU_POWER_OTA_ERR_STATE); /* no session */

    /* Data corrupted in transit: END refuses, the slot stays empty and the old image runs. */
    bad.data[1000] ^= 0x40u;
    assert(begin(&o, 1u, &imgB, IMU_POWER_OTA_HWID, imgB.size) == 0u);
    {
        uint32_t off;
        for (off = 0; off < bad.size; off += 128u)
            assert(chunk(&o, &bad, off, bad.size - off < 128u ? bad.size - off : 128u) == 0u);
    }
    assert(cmd(&o, IMU_POWER_OTA_CMD_END) != 0u && o.last_error == IMU_POWER_OTA_ERR_CRC);
    assert(cmd(&o, IMU_POWER_OTA_CMD_COMMIT) != 0u);
    assert(imu_power_ota_select(&flash) == 0);

    /* Chunk ordering: a gap is refused, a repeat is acknowledged, an odd length is only valid at the end. */
    assert(begin(&o, 1u, &imgB, IMU_POWER_OTA_HWID, imgB.size) == 0u);
    assert(chunk(&o, &imgB, 128u, 128u) == IMU_POWER_DXL_DATA_RANGE && o.last_error == IMU_POWER_OTA_ERR_ORDER);
    assert(chunk(&o, &imgB, 0u, 128u) == 0u);
    assert(chunk(&o, &imgB, 0u, 128u) == 0u && o.next == 128u);
    assert(chunk(&o, &imgB, 128u, 127u) == 0u && o.next == 255u); /* any length is fine; blocks are assembled */
    assert(chunk(&o, &imgB, 255u, 1u) == 0u && o.next == 256u);
    assert(chunk(&o, &imgB, imgB.size, 1u) == IMU_POWER_DXL_DATA_RANGE);
    assert(cmd(&o, IMU_POWER_OTA_CMD_END) != 0u && o.last_error == IMU_POWER_OTA_ERR_STATE); /* incomplete */
    assert(slot_block_programmed(1u, 0u));
    assert(cmd(&o, IMU_POWER_OTA_CMD_ABORT) == 0u);

    /* An odd-sized image: the last byte is padded. */
    {
        image_t odd;
        make_image(&odd, 9u, 1001u, 0x0300u);
        o = session();
        assert(update(&o, 1u, &odd, true) == 0u);
        assert(imu_power_ota_select(&flash) == 1 && slot_holds(1, &odd));
    }
}

/* Cut the power after k flash operations of an update, for every k, then check the board still starts
   something sensible and that an update can be repeated. */
static void sweep(const image_t *from, const image_t *to)
{
    long total, k;
    imu_power_ota_t o;
    erase_all();
    install(0, from, 1u);
    o = session();
    ops_done = 0;
    assert(update(&o, 1u, to, true) == 0u);
    total = ops_done;
    assert(total > 40);
    for (k = 0; k <= total + 1; ++k) {
        int chosen;
        erase_all();
        install(0, from, 1u);
        budget = k; ops_done = 0;
        o = session();
        (void)update(&o, 1u, to, true);
        budget = -1;
        chosen = imu_power_ota_select(&flash);
        if (k >= total) {
            assert(chosen == 1 && slot_holds(1, to));
        } else {
            /* Before the commit marker is written the old image runs; once it is written, the new image
               has been verified in full. Never anything else. */
            assert(chosen == 0 || (chosen == 1 && slot_holds(1, to)));
            if (chosen == 0) assert(slot_holds(0, from));
        }
        o = session();
        assert(update(&o, 1u, to, true) == 0u);
        assert(imu_power_ota_select(&flash) == 1 && slot_holds(1, to));
    }
}

static void test_power_loss(void)
{
    sweep(&imgA, &imgB);
    sweep(&imgB, &imgC);
}

static void test_status_and_ping(void)
{
    imu_power_ota_t o;
    uint8_t p[4] = { 0x00u, 0x03u, 32u, 0u };
    uint8_t out[64];
    imu_power_dxl_frame_t f = { 200u, IMU_POWER_DXL_READ, p, 4u, false };
    size_t n;
    erase_all();
    install(0, &imgA, 1u);
    o = session();
    n = imu_power_ota_handle(&o, &f, out, sizeof(out));
    assert(n == 8u + 1u + 32u + 2u);
    assert(out[8] == 0u && out[9] == IMU_POWER_OTA_MODE_BOOTLOADER && out[9 + 6] == 0xFFu);
    assert(out[9 + 7] == IMU_POWER_OTA_SLOT_CONFIRMED && out[9 + 8] == IMU_POWER_OTA_SLOT_EMPTY);
    f.id = 7u;
    assert(imu_power_ota_handle(&o, &f, out, sizeof(out)) == 0u); /* other IDs and broadcast are ignored */
    f.id = 200u; f.broadcast = true;
    assert(imu_power_ota_handle(&o, &f, out, sizeof(out)) == 0u);
}

int main(void)
{
    make_image(&imgA, 1u, 9000u, 0x0210u);
    make_image(&imgB, 2u, 11000u, 0x0211u);
    make_image(&imgC, 3u, 12000u, 0x0212u);
    test_select_and_trials();
    test_rejections();
    test_status_and_ping();
    test_power_loss();
    puts("ota tests passed");
    return 0;
}
