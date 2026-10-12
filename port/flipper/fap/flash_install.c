/*
 * flipper-doom: install the engine image into the free internal flash gap.
 *
 * Stock firmware no longer links its flash write functions, so this mirrors
 * the locking protocol of furi_hal_flash.c (core2 mutex, HSEM 2/6/7). The one
 * piece apps cannot reach is SHCI_C2_FLASH_EraseActivity, which lets the radio
 * core schedule around an erase; erasing is therefore only done with Bluetooth
 * off, when the radio core has nothing to schedule. Verified on 1.5.1-rc with
 * tools/flashexec.
 */

#include <furi.h>
#include <furi_hal.h>
#include <stm32wbxx.h>
#include <stm32wbxx_ll_hsem.h>

#include "flash_install.h"

#define TAG "DoomFlash"

#define PAGE_SIZE 4096U
#define GAP_END   0x080D7000U // first page of the secure radio area on 1.5.1-rc

#define SEM_FLASH      2 // CFG_HW_FLASH_SEMID
#define SEM_BLOCK_CPU1 6 // CFG_HW_BLOCK_FLASH_REQ_BY_CPU1_SEMID
#define SEM_BLOCK_CPU2 7 // CFG_HW_BLOCK_FLASH_REQ_BY_CPU2_SEMID

#define FLASH_KEY1 0x45670123U
#define FLASH_KEY2 0xCDEF89ABU
#define SR_ERRORS                                                                          \
	(FLASH_SR_OPERR | FLASH_SR_PROGERR | FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_SIZERR | \
	 FLASH_SR_PGSERR | FLASH_SR_MISERR | FLASH_SR_FASTERR | FLASH_SR_RDERR | FLASH_SR_OPTVERR)

// Firmware builds whose code is known to end below FZ_IMAGE_ADDRESS.
static const char* const known_firmware[] = {"1.5.1-rc"};
#define OVERRIDE_FILE EXT_PATH("apps_data/flipper_doom/allow_any_firmware")

static __FuriCriticalInfo crit;


bool flash_gap_is_free(const char** why)
{
	uint32_t secure = FLASH_BASE + ((FLASH->SFR & FLASH_SFR_SFSA) >> FLASH_SFR_SFSA_Pos) * PAGE_SIZE;
	if(secure < GAP_END) {
		*why = "Radio stack overlaps the gap";
		return false;
	}

	const char* version = version_get_version(NULL);
	for(size_t i = 0; i < COUNT_OF(known_firmware); i++)
		if(strcmp(version, known_firmware[i]) == 0) return true;

	Storage* storage = furi_record_open(RECORD_STORAGE);
	bool allowed = storage_file_exists(storage, OVERRIDE_FILE);
	furi_record_close(RECORD_STORAGE);
	if(!allowed) *why = "Untested firmware version";
	return allowed;
}


static uint32_t image_crc(const uint8_t* data, uint32_t size, uint32_t crc)
{
	crc = ~crc;
	for(uint32_t i = 0; i < size; i++) {
		crc ^= data[i];
		for(int b = 0; b < 8; b++)
			crc = (crc >> 1) ^ (0xEDB88320U & -(crc & 1));
	}
	return ~crc;
}


FlashImageState flash_image_check(File* file, fz_image_header_t* hdr, bool read_flash)
{
	storage_file_seek(file, 0, true);
	if(storage_file_read(file, hdr, sizeof(*hdr)) != sizeof(*hdr) || hdr->magic != FZ_IMAGE_MAGIC ||
	   hdr->api_version != FZ_API_VERSION || hdr->image_size > GAP_END - FZ_IMAGE_ADDRESS ||
	   storage_file_size(file) != hdr->image_size)
		return FlashImageBadFile;

	// a page left half-erased by a reset has broken ECC, and reading it raises
	// an NMI the firmware does not handle (the Flipper freezes)
	if(!read_flash) return FlashImageMissing;

	const fz_image_header_t* installed = (const fz_image_header_t*)FZ_IMAGE_ADDRESS;
	if(memcmp(installed, hdr, sizeof(*hdr)) != 0) return FlashImageMissing;

	// header matches; make sure the body was not damaged (or half written)
	uint32_t crc = image_crc((const uint8_t*)FZ_IMAGE_ADDRESS + sizeof(*hdr), hdr->image_size - sizeof(*hdr), 0);
	return crc == hdr->crc32 ? FlashImageOk : FlashImageMissing;
}


static bool wait_not_busy(uint32_t timeout_us)
{
	FuriHalCortexTimer t = furi_hal_cortex_timer_get(timeout_us);
	while(READ_BIT(FLASH->SR, FLASH_SR_BSY | FLASH_SR_CFGBSY))
		if(furi_hal_cortex_timer_is_expired(t)) return false;
	return true;
}


static void flush_caches(void)
{
	CLEAR_BIT(FLASH->ACR, FLASH_ACR_ICEN | FLASH_ACR_DCEN);
	SET_BIT(FLASH->ACR, FLASH_ACR_ICRST | FLASH_ACR_DCRST);
	CLEAR_BIT(FLASH->ACR, FLASH_ACR_ICRST | FLASH_ACR_DCRST);
	SET_BIT(FLASH->ACR, FLASH_ACR_ICEN | FLASH_ACR_DCEN);
}


// furi_hal_flash_begin() without the erase notification; returns inside a
// critical section holding HSEM 2 and 7
static bool flash_begin(void)
{
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


static void flash_end(void)
{
	LL_HSEM_ReleaseLock(HSEM, SEM_BLOCK_CPU2, 0);
	__furi_critical_exit(crit);
	wait_not_busy(100 * 1000);
	SET_BIT(FLASH->CR, FLASH_CR_LOCK);
	LL_HSEM_ReleaseLock(HSEM, SEM_FLASH, 0);
	furi_hal_power_insomnia_exit();
	furi_hal_bt_unlock_core2();
	flush_caches();
}


static bool erase_page(uint32_t address)
{
	if(address < FZ_IMAGE_ADDRESS || address + PAGE_SIZE > GAP_END || address % PAGE_SIZE) return false;
	if(!flash_begin()) return false;

	bool ok = wait_not_busy(100 * 1000);
	if(ok) {
		uint32_t page = (address - FLASH_BASE) / PAGE_SIZE;
		MODIFY_REG(FLASH->CR, FLASH_CR_PNB, (page << FLASH_CR_PNB_Pos) | FLASH_CR_PER | FLASH_CR_STRT);
		ok = wait_not_busy(100 * 1000) && !(FLASH->SR & SR_ERRORS);
		CLEAR_BIT(FLASH->CR, FLASH_CR_PER | FLASH_CR_PNB);
	}
	flash_end();
	return ok;
}


// length: multiple of 8, at most 512 so interrupts stay off for ~6 ms
static bool program(uint32_t address, const uint8_t* data, uint32_t length)
{
	if(address < FZ_IMAGE_ADDRESS || address + length > GAP_END || address % 8 || length % 8) return false;
	if(!flash_begin()) return false;

	bool ok = wait_not_busy(100 * 1000);
	if(ok) {
		SET_BIT(FLASH->CR, FLASH_CR_PG);
		for(uint32_t i = 0; i < length && ok; i += 8) {
			uint32_t lo, hi;
			memcpy(&lo, data + i, 4);
			memcpy(&hi, data + i + 4, 4);
			*(volatile uint32_t*)(address + i) = lo;
			__ISB();
			*(volatile uint32_t*)(address + i + 4) = hi;
			ok = wait_not_busy(10 * 1000) && !(FLASH->SR & SR_ERRORS);
		}
		CLEAR_BIT(FLASH->CR, FLASH_CR_PG);
	}
	flash_end();
	return ok;
}


void trace(const char* fmt, ...); // the loader's log on the SD card

bool flash_image_install(File* file, const fz_image_header_t* hdr, FlashProgress progress, void* context, const char** error)
{
	if(furi_hal_bt_is_active()) {
		*error = "Turn Bluetooth off";
		return false;
	}

	const uint32_t pages = (hdr->image_size + PAGE_SIZE - 1) / PAGE_SIZE;
	uint8_t* buf = malloc(512);

	// The header goes last, so an interrupted install never looks valid.
	bool ok = true;
	for(uint32_t p = 0; p < pages && ok; p++) {
		uint32_t page = FZ_IMAGE_ADDRESS + p * PAGE_SIZE;
		ok = erase_page(page);
		if(!ok) *error = "Flash erase failed";

		for(uint32_t off = 0; off < PAGE_SIZE && ok; off += 512) {
			uint32_t pos = p * PAGE_SIZE + off;
			if(pos >= hdr->image_size) break;
			uint32_t len = hdr->image_size - pos < 512 ? hdr->image_size - pos : 512;
			memset(buf, 0xff, 512);
			storage_file_seek(file, pos, true);
			if(storage_file_read(file, buf, len) != len) {
				ok = false;
				*error = "SD read failed";
				break;
			}
			// skip the header now: an ECC-protected dword can be written only once
			uint32_t skip = pos == 0 ? sizeof(fz_image_header_t) : 0;
			ok = program(page + off + skip, buf + skip, ((len + 7) & ~7U) - skip);
			if(!ok) *error = "Flash write failed";
		}
		trace("page %lu: %s", p + 1, ok ? "ok" : *error);
		if(progress) progress(context, p + 1, pages);
	}

	if(ok) {
		memcpy(buf, hdr, sizeof(*hdr));
		ok = program(FZ_IMAGE_ADDRESS, buf, sizeof(*hdr));
		if(!ok) *error = "Header write failed";
		trace("header: %s", ok ? "ok" : *error);
	}
	free(buf);

	fz_image_header_t check;
	trace("verify");
	if(ok && flash_image_check(file, &check, true) != FlashImageOk) {
		ok = false;
		*error = "Verification failed";
	}
	trace("verified: %s", ok ? "ok" : *error);
	FURI_LOG_I(TAG, "install %s (%lu pages)", ok ? "ok" : *error, pages);
	return ok;
}
