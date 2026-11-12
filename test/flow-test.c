/* End-to-end algorithm test for the optical-flow pipeline, standalone
 * against an installed OBS' libobs — no OBS UI needed.
 *
 * Synthesises two 640x360 frames: a textured 64x64 square on a dark
 * background, shifted +16 px in x between frame A and B. Runs the full
 * shader pipeline (Pack -> Down -> Motion coarse..fine -> SmoothFlow ->
 * Interp at t=0.5) and verifies:
 *   1. the flow field at the square's centre is ~(+16, 0) px
 *   2. the flow field on the background is ~0
 *   3. the interpolated image shows the square at x ~= +8 px (midpoint)
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test-common.h"
#include <graphics/vec2.h>
#include <graphics/vec4.h>

#define W 640
#define H 360
#define SQ 64
#define AX 200 /* square x in frame A */
#define BX 216 /* square x in frame B (+16 px) */
#define SY 150

#define MAX_PYR 6

static float h2f(uint16_t h)
{
	uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 0x1f, man = h & 0x3ff;
	float f;
	if (exp == 0)
		f = ldexpf((float)man, -24);
	else if (exp == 31)
		f = man ? NAN : INFINITY;
	else
		f = ldexpf((float)(man + 1024), (int)exp - 25);
	return sign ? -f : f;
}

static void fill_frame(uint8_t *px, int sq_x)
{
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			uint8_t *p = px + 4 * (y * W + x);
			int inside = (x >= sq_x && x < sq_x + SQ && y >= SY && y < SY + SQ);
			if (inside) {
				int gx = x - sq_x;
				int gy = y - SY;
				p[0] = (uint8_t)(120 + gx * 2);
				p[1] = (uint8_t)(90 + gy * 2);
				p[2] = 200;
			} else {
				p[0] = 24;
				p[1] = 28;
				p[2] = 32;
			}
			p[3] = 255;
		}
	}
}

static gs_effect_t *effect;
static gs_eparam_t *ep(const char *n)
{
	return gs_effect_get_param_by_name(effect, n);
}

static void set_texel(const char *name, uint32_t w, uint32_t h)
{
	struct vec2 v;
	vec2_set(&v, 1.0f / (float)w, 1.0f / (float)h);
	gs_effect_set_vec2(ep(name), &v);
}

static void draw_pass(gs_texrender_t *dst, uint32_t w, uint32_t h, const char *tech, gs_texture_t *spr)
{
	gs_texrender_reset(dst);
	if (!gs_texrender_begin(dst, w, h)) {
		fprintf(stderr, "texrender_begin failed (%s)\n", tech);
		exit(10);
	}
	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	while (gs_effect_loop(effect, tech))
		gs_draw_sprite(spr, 0, w, h);
	gs_texrender_end(dst);
}

int main(int argc, char **argv)
{
	struct test_opts o = test_parse_args(argc, argv);
	test_gs_create(&o);
	effect = test_load_effect(o.effect);

	/* source frames */
	static uint8_t bufa[W * H * 4], bufb[W * H * 4];
	fill_frame(bufa, AX);
	fill_frame(bufb, BX);
	const uint8_t *da = bufa, *db = bufb;
	gs_texture_t *tex_a = gs_texture_create(W, H, GS_RGBA, 1, &da, 0);
	gs_texture_t *tex_b = gs_texture_create(W, H, GS_RGBA, 1, &db, 0);

	/* pyramid setup (mirrors ensure_textures with flow_div=4) */
	uint32_t pw[MAX_PYR], ph[MAX_PYR];
	gs_texrender_t *pyr[MAX_PYR], *fme[MAX_PYR], *fmed[MAX_PYR];
	uint32_t w = W / 4, h = H / 4;
	int levels = 0;
	while (levels < MAX_PYR) {
		pw[levels] = w;
		ph[levels] = h;
		pyr[levels] = gs_texrender_create(GS_RG16F, GS_ZS_NONE);
		fme[levels] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
		fmed[levels] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
		levels++;
		if (w / 2 < 24 || h / 2 < 14)
			break;
		w /= 2;
		h /= 2;
	}
	printf("pyramid: %d levels, base %ux%u\n", levels, pw[0], ph[0]);

	gs_enable_blending(false);
	gs_set_cull_mode(GS_NEITHER);

	/* Pack */
	gs_effect_set_texture(ep("image"), tex_a);
	gs_effect_set_texture(ep("image2"), tex_b);
	set_texel("texel", W, H);
	draw_pass(pyr[0], pw[0], ph[0], "Pack", tex_a);

	/* Down chain */
	for (int l = 1; l < levels; l++) {
		gs_texture_t *src = gs_texrender_get_texture(pyr[l - 1]);
		gs_effect_set_texture(ep("image"), src);
		set_texel("texel", pw[l - 1], ph[l - 1]);
		draw_pass(pyr[l], pw[l], ph[l], "Down", src);
	}

	/* Motion + median, coarse to fine */
	for (int l = levels - 1; l >= 0; l--) {
		gs_texture_t *lvl = gs_texrender_get_texture(pyr[l]);
		int has_prev = l < levels - 1;
		gs_texture_t *prev = has_prev ? gs_texrender_get_texture(fmed[l + 1]) : lvl;

		gs_effect_set_texture(ep("image"), lvl);
		gs_effect_set_texture(ep("prev_flow"), prev);
		gs_effect_set_texture(ep("temporal_flow"), lvl);
		set_texel("texel", pw[l], ph[l]);
		set_texel("flow_texel", pw[has_prev ? l + 1 : l], ph[has_prev ? l + 1 : l]);
		gs_effect_set_float(ep("has_prev"), has_prev ? 1.0f : 0.0f);
		gs_effect_set_float(ep("has_temporal"), 0.0f);
		gs_effect_set_float(ep("fine_pass"), (l == 0) ? 1.0f : 0.0f);
		draw_pass(fme[l], pw[l], ph[l], "Motion", lvl);

		gs_texture_t *raw = gs_texrender_get_texture(fme[l]);
		gs_effect_set_texture(ep("image"), raw);
		set_texel("texel", pw[l], ph[l]);
		draw_pass(fmed[l], pw[l], ph[l], "SmoothFlow", raw);
	}

	/* read back flow */
	gs_stagesurf_t *fstage = gs_stagesurface_create(pw[0], ph[0], GS_RGBA16F);
	gs_stage_texture(fstage, gs_texrender_get_texture(fmed[0]));
	uint8_t *data;
	uint32_t linesize;
	if (!gs_stagesurface_map(fstage, &data, &linesize)) {
		fprintf(stderr, "flow map failed\n");
		return 4;
	}

	/* square centre in flow-texture coords */
	int cx = (AX + SQ / 2) * (int)pw[0] / W;
	int cy = (SY + SQ / 2) * (int)ph[0] / H;
	const uint16_t *fp = (const uint16_t *)(data + (size_t)cy * linesize) + (size_t)cx * 4;
	float vfx = h2f(fp[0]) * W, vfy = h2f(fp[1]) * H;
	float vbx = h2f(fp[2]) * W, vby = h2f(fp[3]) * H;

	/* background probe (far from square) */
	int bx = 40 * (int)pw[0] / W, by = 40 * (int)ph[0] / H;
	const uint16_t *bp = (const uint16_t *)(data + (size_t)by * linesize) + (size_t)bx * 4;
	float bfx = h2f(bp[0]) * W, bfy = h2f(bp[1]) * H;
	gs_stagesurface_unmap(fstage);

	printf("flow @ square centre: fwd=(%+.2f, %+.2f) px  bwd=(%+.2f, %+.2f) px (expect fwd ~ +16,0; bwd ~ -16,0)\n",
	       vfx, vfy, vbx, vby);
	printf("flow @ background:    fwd=(%+.2f, %+.2f) px (expect ~0)\n", bfx, bfy);

	/* Interp at t = 0.5 */
	gs_texrender_t *out = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	struct vec2 px;
	gs_effect_set_texture(ep("image"), tex_a);
	gs_effect_set_texture(ep("image2"), tex_b);
	gs_effect_set_texture(ep("flow_tex"), gs_texrender_get_texture(fmed[0]));
	vec2_set(&px, (float)W, (float)H);
	gs_effect_set_vec2(ep("frame_px"), &px);
	gs_effect_set_float(ep("t_phase"), 0.5f);
	gs_effect_set_float(ep("occl_k"), 7.0f);
	gs_effect_set_float(ep("strength"), 1.0f);
	/* same as the filter: half a flow texel in source px */
	gs_effect_set_float(ep("flow_quant"), 0.5f * (float)W / (float)pw[0]);
	draw_pass(out, W, H, "Interp", tex_a);

	gs_stagesurf_t *ostage = gs_stagesurface_create(W, H, GS_RGBA);
	gs_stage_texture(ostage, gs_texrender_get_texture(out));
	if (!gs_stagesurface_map(ostage, &data, &linesize)) {
		fprintf(stderr, "out map failed\n");
		return 5;
	}

	/* scan the square's centre row for the bright object */
	int row = SY + SQ / 2;
	const uint8_t *r = data + (size_t)row * linesize;
	int first = -1, last = -1;
	for (int x = 0; x < W; x++) {
		int lumv = (r[x * 4] + r[x * 4 + 1] + r[x * 4 + 2]) / 3;
		if (lumv > 70) {
			if (first < 0)
				first = x;
			last = x;
		}
	}
	gs_stagesurface_unmap(ostage);

	int exp_first = (AX + BX) / 2; /* 208 */
	printf("interp t=0.5: object spans x=[%d..%d] (expected left edge ~%d, span ~%d)\n", first, last,
	       exp_first, SQ);

	int ok = 1;
	if (fabsf(vfx - 16.0f) > 4.0f || fabsf(vfy) > 3.0f) {
		printf("FAIL: forward flow wrong\n");
		ok = 0;
	}
	if (fabsf(vbx + 16.0f) > 4.0f || fabsf(vby) > 3.0f) {
		printf("FAIL: backward flow wrong\n");
		ok = 0;
	}
	if (fabsf(bfx) > 2.0f || fabsf(bfy) > 2.0f) {
		printf("FAIL: background flow not ~0\n");
		ok = 0;
	}
	if (first < 0 || abs(first - exp_first) > 5 || abs((last - first + 1) - SQ) > 12) {
		printf("FAIL: interpolated object position/size wrong\n");
		ok = 0;
	}

	printf(ok ? "ALL CHECKS PASSED\n" : "TEST FAILED\n");

	gs_leave_context();
	return ok ? 0 : 1;
}
