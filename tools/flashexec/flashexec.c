// Copyright (C) 2026 HedgDifuse
// SPDX-License-Identifier: GPL-2.0-or-later
// Proof of concept: a FAP writes code into the unused internal flash gap and
// executes it in place (XIP). Mirrors the locking protocol of the official
// furi_hal_flash.c (HSEM 2/6/7 + core2 mutex), which stock firmware no longer
// links in.
//
// Command is read from /ext/apps_data/flashexec.cmd: "write", "run" or "erase".
// Report goes to /ext/apps_data/flashexec.txt.
#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>
#include <stm32wbxx.h>
#include <stm32wbxx_ll_hsem.h>

#define TAG "flashexec"

#define PAGE_SIZE   4096U
#define TARGET      0x080D6000U // last page below the secure radio area
#define GAP_START   0x080BD000U // first page after stock 1.5.1-rc firmware
#define PATTERN_OFS 64U
#define PATTERN_LEN 1024U

#define SEM_FLASH     2 // CFG_HW_FLASH_SEMID
#define SEM_BLOCK_CPU1 6 // CFG_HW_BLOCK_FLASH_REQ_BY_CPU1_SEMID
#define SEM_BLOCK_CPU2 7 // CFG_HW_BLOCK_FLASH_REQ_BY_CPU2_SEMID

#define FLASH_KEY1 0x45670123U
#define FLASH_KEY2 0xCDEF89ABU
#define SR_ERRORS                                                                        \
    (FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR |              \
     FLASH_SR_SIZERR | FLASH_SR_PGSERR | FLASH_SR_MISERR | FLASH_SR_FASTERR | FLASH_SR_RDERR | \
     FLASH_SR_OPTVERR)

// int f(int a, int b, int (*cb)(int)) { return cb(a * b + 42); }
static const uint16_t test_code[8] = {
    0xB510, // push {r4, lr}
    0x4348, // muls r0, r1
    0x302A, // adds r0, #42
    0x4790, // blx  r2
    0xBD10, // pop  {r4, pc}
    0xBF00, 0xBF00, 0xBF00, // nop padding to 16 bytes
};
static const char magic[16] = "FLASHEXEC v1\0\0\0";

typedef int (*TestFn)(int, int, int (*)(int));

static int callback_from_flash(int x) {
    return x + 1000;
}

static FuriString* report;
static __FuriCriticalInfo crit; // held between flash_begin() and flash_end()
#define R(...) furi_string_cat_printf(report, __VA_ARGS__)

static bool page_is_erased(uint32_t page) {
    for(uint32_t a = page; a < page + PAGE_SIZE; a += 4)
        if(*(volatile uint32_t*)a != 0xFFFFFFFFU) return false;
    return true;
}

static bool wait_not_busy(uint32_t timeout_us) {
    FuriHalCortexTimer t = furi_hal_cortex_timer_get(timeout_us);
    while(READ_BIT(FLASH->SR, FLASH_SR_BSY | FLASH_SR_CFGBSY)) {
        if(furi_hal_cortex_timer_is_expired(t)) return false;
    }
    return true;
}

static void flush_caches(void) {
    CLEAR_BIT(FLASH->ACR, FLASH_ACR_ICEN | FLASH_ACR_DCEN);
    SET_BIT(FLASH->ACR, FLASH_ACR_ICRST | FLASH_ACR_DCRST);
    CLEAR_BIT(FLASH->ACR, FLASH_ACR_ICRST | FLASH_ACR_DCRST);
    SET_BIT(FLASH->ACR, FLASH_ACR_ICEN | FLASH_ACR_DCEN);
}

// Same sequence as furi_hal_flash_begin()/begin_with_core2(), minus
// SHCI_C2_FLASH_EraseActivity which is not available to apps.
// On success returns inside a critical section holding HSEM 2 and 7.
static bool flash_begin(void) {
    furi_hal_bt_lock_core2();
    furi_hal_power_insomnia_enter();

    FuriHalCortexTimer t = furi_hal_cortex_timer_get(1000 * 1000);
    while(LL_HSEM_1StepLock(HSEM, SEM_FLASH) != 0) {
        if(furi_hal_cortex_timer_is_expired(t)) goto fail_sem;
        furi_thread_yield();
    }

    if(READ_BIT(FLASH->CR, FLASH_CR_LOCK)) {
        WRITE_REG(FLASH->KEYR, FLASH_KEY1);
        __ISB();
        WRITE_REG(FLASH->KEYR, FLASH_KEY2);
    }
    if(READ_BIT(FLASH->CR, FLASH_CR_LOCK)) goto fail_unlock;

    furi_delay_us(5);

    t = furi_hal_cortex_timer_get(3000 * 1000);
    while(true) {
        while(READ_BIT(FLASH->SR, FLASH_SR_PESD)) {
            if(furi_hal_cortex_timer_is_expired(t)) goto fail_unlock;
            furi_thread_yield();
        }
        crit = __furi_critical_enter();
        if(LL_HSEM_IsSemaphoreLocked(HSEM, SEM_BLOCK_CPU1) ||
           LL_HSEM_1StepLock(HSEM, SEM_BLOCK_CPU2) != 0) {
            __furi_critical_exit(crit);
            if(furi_hal_cortex_timer_is_expired(t)) goto fail_unlock;
            furi_thread_yield();
            continue;
        }
        break;
    }
    // stale error flags would make the next operation fail
    WRITE_REG(FLASH->SR, SR_ERRORS);
    return true;

fail_unlock:
    SET_BIT(FLASH->CR, FLASH_CR_LOCK);
    LL_HSEM_ReleaseLock(HSEM, SEM_FLASH, 0);
fail_sem:
    furi_hal_power_insomnia_exit();
    furi_hal_bt_unlock_core2();
    return false;
}

static void flash_end(void) {
    LL_HSEM_ReleaseLock(HSEM, SEM_BLOCK_CPU2, 0);
    __furi_critical_exit(crit);
    wait_not_busy(100 * 1000);
    SET_BIT(FLASH->CR, FLASH_CR_LOCK);
    LL_HSEM_ReleaseLock(HSEM, SEM_FLASH, 0);
    furi_hal_power_insomnia_exit();
    furi_hal_bt_unlock_core2();
    flush_caches();
}

static bool program(uint32_t addr, const uint8_t* data, size_t len) {
    if(addr < GAP_START || addr % 8 || len % 8 || addr + len > TARGET + PAGE_SIZE) return false;
    if(!flash_begin()) {
        R("flash_begin failed\n");
        return false;
    }
    bool ok = wait_not_busy(100 * 1000);
    uint32_t sr = 0;
    if(ok) {
        SET_BIT(FLASH->CR, FLASH_CR_PG);
        for(size_t i = 0; i < len && ok; i += 8) {
            uint32_t lo, hi;
            memcpy(&lo, data + i, 4);
            memcpy(&hi, data + i + 4, 4);
            *(volatile uint32_t*)(addr + i) = lo;
            __ISB();
            *(volatile uint32_t*)(addr + i + 4) = hi;
            ok = wait_not_busy(10 * 1000);
            sr = FLASH->SR & SR_ERRORS;
            if(sr) ok = false;
        }
        CLEAR_BIT(FLASH->CR, FLASH_CR_PG);
    }
    flash_end();
    if(!ok) R("program failed, SR errors=0x%08lX\n", sr);
    return ok;
}

static bool erase(uint32_t page_addr) {
    if(page_addr != TARGET) return false;
    if(!flash_begin()) {
        R("flash_begin failed\n");
        return false;
    }
    bool ok = wait_not_busy(100 * 1000);
    if(ok) {
        uint32_t page = (page_addr - FLASH_BASE) / PAGE_SIZE;
        MODIFY_REG(
            FLASH->CR, FLASH_CR_PNB, (page << FLASH_CR_PNB_Pos) | FLASH_CR_PER | FLASH_CR_STRT);
        ok = wait_not_busy(100 * 1000) && !(FLASH->SR & SR_ERRORS);
        CLEAR_BIT(FLASH->CR, FLASH_CR_PER | FLASH_CR_PNB);
    }
    flash_end();
    return ok;
}

static uint8_t pattern_byte(uint32_t i) {
    return (uint8_t)(i * 7U ^ 0x5AU);
}

static void cmd_write(void) {
    if(!page_is_erased(TARGET)) {
        R("ABORT: target page is not erased, refusing to write\n");
        return;
    }
    static uint8_t buf[PATTERN_OFS + PATTERN_LEN];
    memset(buf, 0xFF, sizeof(buf));
    memcpy(buf, test_code, sizeof(test_code));
    memcpy(buf + 16, magic, sizeof(magic));
    for(uint32_t i = 0; i < PATTERN_LEN; i++) buf[PATTERN_OFS + i] = pattern_byte(i);

    uint32_t t0 = DWT->CYCCNT;
    bool ok = program(TARGET, buf, sizeof(buf));
    uint32_t us = (DWT->CYCCNT - t0) / furi_hal_cortex_instructions_per_microsecond();
    R("write %u bytes: %s, %lu us\n", sizeof(buf), ok ? "OK" : "FAILED", us);
}

static void cmd_run(void) {
    if(memcmp((const void*)(TARGET + 16), magic, sizeof(magic)) != 0) {
        R("ABORT: no test code at target\n");
        return;
    }
    uint32_t bad = 0;
    for(uint32_t i = 0; i < PATTERN_LEN; i++)
        if(*(volatile uint8_t*)(TARGET + PATTERN_OFS + i) != pattern_byte(i)) bad++;
    R("pattern check: %lu bad bytes of %u\n", bad, PATTERN_LEN);

    TestFn fn = (TestFn)(TARGET | 1U); // thumb
    int result = fn(6, 7, callback_from_flash);
    R("fn(6,7,cb) executed from flash 0x%08lX = %d (expected 1084): %s\n",
      (uint32_t)TARGET, result, result == 1084 ? "OK" : "WRONG");
}

static void cmd_erase(void) {
    if(furi_hal_bt_is_active()) {
        R("ABORT: Bluetooth is active, turn it off in Settings first\n");
        return;
    }
    uint32_t t0 = DWT->CYCCNT;
    bool ok = erase(TARGET);
    uint32_t us = (DWT->CYCCNT - t0) / furi_hal_cortex_instructions_per_microsecond();
    R("erase page 0x%08lX: %s, %lu us, now erased=%d\n", (uint32_t)TARGET, ok ? "OK" : "FAILED",
      us, page_is_erased(TARGET));
}

int32_t flashexec_app(void* p) {
    UNUSED(p);
    report = furi_string_alloc();
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(storage);

    char cmd[16] = {0};
    if(storage_file_open(f, EXT_PATH("apps_data/flashexec.cmd"), FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_read(f, cmd, sizeof(cmd) - 1);
    }
    storage_file_close(f);
    for(char* c = cmd; *c; c++)
        if(*c == '\r' || *c == '\n') *c = 0;

    uint32_t sfsa_addr = FLASH_BASE + ((FLASH->SFR & FLASH_SFR_SFSA) >> FLASH_SFR_SFSA_Pos) * PAGE_SIZE;
    R("cmd=%s bt_alive=%d bt_active=%d secure_start=0x%08lX\n", cmd, furi_hal_bt_is_alive(),
      furi_hal_bt_is_active(), sfsa_addr);

    if(TARGET + PAGE_SIZE > sfsa_addr) {
        R("ABORT: target overlaps secure area\n");
    } else if(!strcmp(cmd, "write")) {
        cmd_write();
    } else if(!strcmp(cmd, "run")) {
        cmd_run();
    } else if(!strcmp(cmd, "erase")) {
        cmd_erase();
    } else {
        R("unknown command\n");
    }

    if(storage_file_open(f, EXT_PATH("apps_data/flashexec.txt"), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(f, furi_string_get_cstr(report), furi_string_size(report));
    }
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    FURI_LOG_I(TAG, "%s", furi_string_get_cstr(report));
    furi_string_free(report);
    return 0;
}
