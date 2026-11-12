/* Performance measurement at 1080p on the real pipeline:
 *  - full flow computation (runs once per NEW source frame, e.g. 30x/s)
 *  - interp/warp pass alone (runs once per canvas tick, e.g. 60x/s)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "test-common.h"
#include <graphics/vec2.h>
#include <graphics/vec4.h>

#define W 1920
#define H 1080
#define MAX_PYR 6
#define ITERS 60

static gs_effect_t *effect;

static void set_texel(const char *name, uint32_t w, uint32_t h)
{
	struct vec2 v;
	vec2_set(&v, 1.0f / (float)w, 1.0f / (float)h);
	gs_effect_set_vec2(gs_effect_get_param_by_name(effect, name), &v);
}

static gs_eparam_t *ep(const char *n)
{
	return gs_effect_get_param_by_name(effect, n);
}

static void draw_pass(gs_texrender_t *dst, uint32_t w, uint32_t h, const char *tech, gs_texture_t *spr)
{
	gs_texrender_reset(dst);
	if (!gs_texrender_begin(dst, w, h))
		exit(10);
	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	while (gs_effect_loop(effect, tech))
		gs_draw_sprite(spr, 0, w, h);
	gs_texrender_end(dst);
}

/* force the GPU to finish all queued work */
static void gpu_sync(gs_stagesurf_t *sync_stage, gs_texture_t *tex)
{
	uint8_t *data;
	uint32_t linesize;
	gs_stage_texture(sync_stage, tex);
	if (gs_stagesurface_map(sync_stage, &data, &linesize))
		gs_stagesurface_unmap(sync_stage);
}

int main(int argc, char **argv)
{
	struct test_opts o = test_parse_args(argc, argv);
	test_gs_create(&o);
	effect = test_load_effect(o.effect);

	/* noisy full-HD frames (worst case for matching: everything moves) */
	static uint8_t bufa[W * H * 4], bufb[W * H * 4];
	srand(1);
	for (size_t i = 0; i < sizeof(bufa); i += 4) {
		bufa[i] = rand() & 255;
		bufa[i + 1] = rand() & 255;
		bufa[i + 2] = rand() & 255;
		bufa[i + 3] = 255;
	}
	memcpy(bufb, bufa, sizeof(bufa));
	/* shift b by 6 px */
	for (int y = 0; y < H; y++)
		memmove(bufb + (size_t)y * W * 4 + 24, bufa + (size_t)y * W * 4, (W - 6) * 4);

	const uint8_t *da = bufa, *db = bufb;
	gs_texture_t *tex_a = gs_texture_create(W, H, GS_RGBA, 1, &da, 0);
	gs_texture_t *tex_b = gs_texture_create(W, H, GS_RGBA, 1, &db, 0);

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
	gs_texrender_t *out = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *sync_flow = gs_stagesurface_create(W / 4, H / 4, GS_RGBA16F);
	gs_stagesurf_t *sync_out = gs_stagesurface_create(W, H, GS_RGBA);

	gs_enable_blending(false);
	gs_set_cull_mode(GS_NEITHER);

	printf("pyramid: %d levels, base %ux%u\n", levels, pw[0], ph[0]);

#define RUN_FLOW()                                                                                        \
	do {                                                                                              \
		gs_effect_set_texture(ep("image"), tex_a);                                                \
		gs_effect_set_texture(ep("image2"), tex_b);                                               \
		set_texel("texel", W, H);                                                                 \
		draw_pass(pyr[0], pw[0], ph[0], "Pack", tex_a);                                           \
		for (int l = 1; l < levels; l++) {                                                        \
			gs_texture_t *src = gs_texrender_get_texture(pyr[l - 1]);                         \
			gs_effect_set_texture(ep("image"), src);                                          \
			set_texel("texel", pw[l - 1], ph[l - 1]);                                         \
			draw_pass(pyr[l], pw[l], ph[l], "Down", src);                                     \
		}                                                                                         \
		for (int l = levels - 1; l >= 0; l--) {                                                   \
			gs_texture_t *lvl = gs_texrender_get_texture(pyr[l]);                             \
			int hp = l < levels - 1;                                                          \
			gs_texture_t *prev = hp ? gs_texrender_get_texture(fmed[l + 1]) : lvl;            \
			gs_effect_set_texture(ep("image"), lvl);                                          \
			gs_effect_set_texture(ep("prev_flow"), prev);                                     \
			gs_effect_set_texture(ep("temporal_flow"), lvl);                                  \
			set_texel("texel", pw[l], ph[l]);                                                 \
			set_texel("flow_texel", pw[hp ? l + 1 : l], ph[hp ? l + 1 : l]);                  \
			gs_effect_set_float(ep("has_prev"), hp ? 1.0f : 0.0f);                            \
			gs_effect_set_float(ep("has_temporal"), 0.0f);                                    \
			gs_effect_set_float(ep("fine_pass"), (l == 0) ? 1.0f : 0.0f);                     \
			draw_pass(fme[l], pw[l], ph[l], "Motion", lvl);                                   \
			gs_texture_t *raw = gs_texrender_get_texture(fme[l]);                             \
			gs_effect_set_texture(ep("image"), raw);                                          \
			set_texel("texel", pw[l], ph[l]);                                                 \
			draw_pass(fmed[l], pw[l], ph[l], "SmoothFlow", raw);                              \
		}                                                                                         \
	} while (0)

#define RUN_INTERP()                                                                                      \
	do {                                                                                              \
		struct vec2 px;                                                                           \
		gs_effect_set_texture(ep("image"), tex_a);                                                \
		gs_effect_set_texture(ep("image2"), tex_b);                                               \
		gs_effect_set_texture(ep("flow_tex"), gs_texrender_get_texture(fmed[0]));                 \
		vec2_set(&px, (float)W, (float)H);                                                        \
		gs_effect_set_vec2(ep("frame_px"), &px);                                                  \
		gs_effect_set_float(ep("t_phase"), 0.5f);                                                 \
		gs_effect_set_float(ep("occl_k"), 7.0f);                                                  \
		gs_effect_set_float(ep("strength"), 1.0f);                                                \
		gs_effect_set_float(ep("flow_quant"), 0.5f * (float)W / (float)pw[0]);                    \
		draw_pass(out, W, H, "Interp", tex_a);                                                    \
	} while (0)

	/* warm-up (shader compile at first use etc.) */
	RUN_FLOW();
	RUN_INTERP();
	gpu_sync(sync_out, gs_texrender_get_texture(out));

	double t0 = test_now_ms();
	for (int i = 0; i < ITERS; i++)
		RUN_FLOW();
	gpu_sync(sync_flow, gs_texrender_get_texture(fmed[0]));
	double flow_ms = (test_now_ms() - t0) / ITERS;

	t0 = test_now_ms();
	for (int i = 0; i < ITERS; i++)
		RUN_INTERP();
	gpu_sync(sync_out, gs_texrender_get_texture(out));
	double interp_ms = (test_now_ms() - t0) / ITERS;

	printf("1080p flow computation (per new source frame): %.2f ms\n", flow_ms);
	printf("1080p interp/warp pass (per canvas tick):      %.2f ms\n", interp_ms);
	printf("=> at 30fps source in 60fps canvas: ~%.2f ms/s GPU budget used per second: %.0f ms\n",
	       flow_ms + interp_ms, flow_ms * 30.0 + interp_ms * 60.0);

	gs_leave_context();
	return 0;
}
