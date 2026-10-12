// Read-only probe: is the flash gap between firmware and the secure radio
// area erased (0xFF)? Also reports heap seen from inside a FAP.
// Result: /ext/apps_data/flashprobe.txt
#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>
#include <stm32wbxx.h>

#define PAGE 4096U
#define SCAN_FROM 0x080B0000U

int32_t flashprobe_app(void* p) {
    UNUSED(p);
    uint32_t sfsa = (FLASH->SFR & FLASH_SFR_SFSA) >> FLASH_SFR_SFSA_Pos;
    uint32_t secure = FLASH_BASE + sfsa * PAGE;

    // last non-0xFF byte below the secure area = end of used flash
    uint32_t last_used = 0;
    for(uint32_t a = SCAN_FROM; a < secure; a += 4) {
        if(*(volatile uint32_t*)a != 0xFFFFFFFFU) last_used = a;
    }
    uint32_t first_free_page = (last_used / PAGE + 1) * PAGE;

    FuriString* s = furi_string_alloc_printf(
        "sfsa_page=0x%02lX secure_start=0x%08lX\n"
        "last_used_word=0x%08lX first_free_page=0x%08lX\n"
        "erased_bytes=%lu (%lu KB)\n"
        "heap_free=%u heap_max_block=%u\n",
        sfsa, secure, last_used, first_free_page,
        secure - first_free_page, (secure - first_free_page) / 1024,
        memmgr_get_free_heap(), memmgr_heap_get_max_free_block());

    // per-page map: count of non-erased words plus a hex peek
    furi_string_cat_printf(s, "pages from 0x080BC000 (page, used words, first 16 bytes):\n");
    for(uint32_t pg = 0x080BC000U; pg < secure; pg += PAGE) {
        uint32_t used = 0;
        for(uint32_t a = pg; a < pg + PAGE; a += 4)
            if(*(volatile uint32_t*)a != 0xFFFFFFFFU) used++;
        furi_string_cat_printf(s, "%08lX %4lu ", pg, used);
        for(int i = 0; i < 16; i++) furi_string_cat_printf(s, "%02X", *(volatile uint8_t*)(pg + i));
        furi_string_cat_printf(s, "\n");
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, EXT_PATH("apps_data/flashprobe.txt"), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(f, furi_string_get_cstr(s), furi_string_size(s));
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    FURI_LOG_I("flashprobe", "%s", furi_string_get_cstr(s));
    furi_string_free(s);
    return 0;
}
