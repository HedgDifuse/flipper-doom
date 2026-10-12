/*
 * flipper-doom: demand pager for read-only WAD data.
 *
 * There is no MMU, so instead of keeping the WAD directory and the SEGS, NODES,
 * BLOCKMAP and REJECT lumps in the zone, the engine copies the records it needs
 * out of a small LRU cache of 512-byte WAD file pages, read from the SD card
 * on a miss.
 */

#ifndef FZ_PAGER_H
#define FZ_PAGER_H

#include <stdint.h>

#if !defined PG_PAGE_SIZE
#define PG_PAGE_SIZE 512
#endif

#if !defined PG_SLOTS
#define PG_SLOTS 16
#endif

// Forget every cached page.
void PG_Flush(void);

// zone kept free for play when the level setup lends the rest to the cache
#if !defined PG_ZONE_RESERVE
#define PG_ZONE_RESERVE (4 * 1024L) // and PG_ReleasePages when it runs out
#endif

// More slots in memory the caller lends (the zone a loaded level leaves
// free), until PG_RemovePages; their pages are forgotten then.
void PG_AddPages(void* memory, uint32_t bytes);
void PG_RemovePages(void);

// When the zone runs out: give the added pages back (Z_Free their block);
// false if there were none.
boolean PG_ReleasePages(void);

// Copy length bytes at WAD file position pos into dest.
void PG_Read(uint32_t pos, void* dest, uint16_t length);

// Statistics for tuning: page misses and bytes read.
extern uint32_t pg_misses, pg_bytes;

#endif
