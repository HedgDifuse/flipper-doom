// How fast can the SD card serve a demand-paging cache? Measures random reads
// of various sizes from a WAD-sized file, plus sequential throughput.
// Report: /ext/apps_data/sdbench.txt
#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>

#define TEST_FILE EXT_PATH("apps_data/sdbench.bin")
#define FILE_SIZE (1536U * 1024U)
#define CHUNK     4096U

static uint8_t buf[CHUNK];

static uint32_t now_us(void) {
    return DWT->CYCCNT / furi_hal_cortex_instructions_per_microsecond();
}

static uint32_t rng_state = 12345;
static uint32_t rnd(void) {
    rng_state = rng_state * 1103515245U + 12345U;
    return rng_state >> 8;
}

int32_t sdbench_app(void* p) {
    UNUSED(p);
    FuriString* r = furi_string_alloc();
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);

    // create the test file once
    FileInfo info;
    if(storage_common_stat(storage, TEST_FILE, &info) != FSE_OK || info.size != FILE_SIZE) {
        uint32_t t0 = now_us();
        storage_file_open(f, TEST_FILE, FSAM_WRITE, FSOM_CREATE_ALWAYS);
        for(uint32_t i = 0; i < CHUNK; i++) buf[i] = (uint8_t)i;
        for(uint32_t done = 0; done < FILE_SIZE; done += CHUNK) storage_file_write(f, buf, CHUNK);
        storage_file_close(f);
        furi_string_cat_printf(r, "created %u KB in %lu ms\n", FILE_SIZE / 1024, (now_us() - t0) / 1000);
    }

    storage_file_open(f, TEST_FILE, FSAM_READ, FSOM_OPEN_EXISTING);

    // sequential
    uint32_t t0 = now_us();
    storage_file_seek(f, 0, true);
    for(uint32_t done = 0; done < 512U * 1024U; done += CHUNK) storage_file_read(f, buf, CHUNK);
    uint32_t us = now_us() - t0;
    furi_string_cat_printf(r, "sequential 512 KB in 4 KB reads: %lu ms = %lu KB/s\n", us / 1000, 512U * 1000000U / us);

    // random reads, file kept open
    static const uint16_t sizes[] = {32, 128, 512, 1024, 2048, 4096};
    for(size_t s = 0; s < COUNT_OF(sizes); s++) {
        const uint32_t n = 200;
        uint32_t worst = 0;
        t0 = now_us();
        for(uint32_t i = 0; i < n; i++) {
            uint32_t off = rnd() % (FILE_SIZE - sizes[s]);
            uint32_t t1 = now_us();
            storage_file_seek(f, off, true);
            storage_file_read(f, buf, sizes[s]);
            uint32_t d = now_us() - t1;
            if(d > worst) worst = d;
        }
        us = now_us() - t0;
        furi_string_cat_printf(
            r, "random %4u B: avg %4lu us, worst %5lu us\n", sizes[s], us / n, worst);
    }

    // small reads that stay inside one 512 B sector (FatFS sector buffer hit?)
    t0 = now_us();
    for(uint32_t i = 0; i < 200; i++) {
        storage_file_seek(f, 4096 + (i % 16) * 32, true);
        storage_file_read(f, buf, 32);
    }
    furi_string_cat_printf(r, "same-sector 32 B: avg %lu us\n", (now_us() - t0) / 200);

    // the same random 512 B reads, sector aligned like the Doom pager's pages
    t0 = now_us();
    for(uint32_t i = 0; i < 200; i++) {
        storage_file_seek(f, (rnd() % (FILE_SIZE / 512)) * 512, true);
        storage_file_read(f, buf, 512);
    }
    furi_string_cat_printf(r, "random aligned 512 B (file API): avg %lu us\n", (now_us() - t0) / 200);

    // raw sectors through the HAL, no FatFS and no storage thread: count 1
    // goes through the firmware's sector cache, 2+ does not
    static uint32_t raw[4 * 512 / 4];
    static const uint8_t counts[] = {1, 2, 4};
    for(size_t c = 0; c < COUNT_OF(counts); c++) {
        uint32_t worst = 0, fails = 0;
        t0 = now_us();
        for(uint32_t i = 0; i < 200; i++) {
            uint32_t sector = 200000 + rnd() % 100000; // the data area, read only
            uint32_t t1 = now_us();
            if(furi_hal_sd_read_blocks(raw, sector, counts[c]) != FuriStatusOk) fails++;
            uint32_t d = now_us() - t1;
            if(d > worst) worst = d;
        }
        furi_string_cat_printf(
            r, "raw HAL %u sector(s): avg %lu us, worst %lu us, %lu failed\n", counts[c], (now_us() - t0) / 200, worst,
            fails);
    }

    storage_file_close(f);
    if(storage_file_open(f, EXT_PATH("apps_data/sdbench.txt"), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(f, furi_string_get_cstr(r), furi_string_size(r));
        storage_file_close(f);
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    FURI_LOG_I("sdbench", "%s", furi_string_get_cstr(r));
    furi_string_free(r);
    return 0;
}
