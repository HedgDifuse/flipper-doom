/*
 * flipper-doom: luminance -> LCD gray level table, shared by the loader
 * (screen calibration) and the simulator. No engine dependencies.
 *
 * Doom luminance is 6 bits with the tone curve already applied (63 = white).
 * The darkness 1 - lum/63 is bent by gamma to match how the panel responds to
 * blinking (calibrated per device), then scaled to levels 0..period. Each
 * entry is level * 16 + fraction, the fraction feeding an ordered dither
 * between neighbouring levels.
 */

#ifndef FZ_GRAYMAP_H
#define FZ_GRAYMAP_H

#include <stdint.h>

#define FZ_GRAY_MAX_PERIOD 8

// x^y for x in (0, 1], to about 0.5%: plenty for a table quantized to 1/16
// of a level, and it keeps libm's powf (2 KB) out of the RAM-loaded app.
static inline float fz_powf(float x, float y)
{
	if (x <= 0.0f)
		return 0.0f;
	union { float f; int32_t i; } u = {x};
	int32_t e = ((u.i >> 23) & 0xff) - 127;
	u.i = (u.i & 0x007fffff) | 0x3f800000; // mantissa m in [1, 2)
	float m = u.f;
	float log2x = e + (-0.34484843f * m + 2.02466578f) * m - 1.67487759f;

	float p = y * log2x; // 2^p, p <= 0
	int32_t n = (int32_t)p;
	if (p < n)
		n--;
	if (n < -126)
		return 0.0f;
	float f = p - n; // [0, 1)
	u.f = 1.0f + f * (0.6565f + f * 0.3435f);
	u.i += n << 23;
	return u.f;
}

static inline void fz_build_lum_to_level(uint8_t lut[64], uint8_t period, float gamma)
{
	for (int lum = 0; lum < 64; lum++)
	{
		float darkness = fz_powf(1.0f - lum / 63.0f, gamma);
		int v = (int)(darkness * period * 16.0f + 0.5f);
		lut[lum] = v > period * 16 ? period * 16 : v;
	}
}

#endif
