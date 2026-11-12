/* Step-by-step pipeline bisection: Copy pass, Pack luma, coarse Motion. */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "test-common.h"
#include <graphics/vec2.h>
#include <graphics/vec4.h>

#define W 640
#define H 360

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

static gs_effect_t *effect;

static void set_texel(const char *name, uint32_t w, uint32_t h)
{
	struct vec2 v;
	vec2_set(&v, 1.0f / (float)w, 1.0f / (float)h);
	gs_effect_set_vec2(gs_effect_get_param_by_name(effect, name), &v);
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

	/* frame A: bright square at 200..264, frame B: shifted +16 */
	static uint8_t bufa[W * H * 4], bufb[W * H * 4];
	for (int y = 0; y < H; y++)
		for (int x = 0; x < W; x++) {
			uint8_t *pa = bufa + 4 * (y * W + x);
			uint8_t *pb = bufb + 4 * (y * W + x);
			int ina = (x >= 200 && x < 264 && y >= 150 && y < 214);
			int inb = (x >= 216 && x < 280 && y >= 150 && y < 214);
			pa[0] = pa[1] = pa[2] = ina ? 220 : 25;
			pb[0] = pb[1] = pb[2] = inb ? 220 : 25;
			pa[3] = pb[3] = 255;
		}
	const uint8_t *da = bufa, *db = bufb;
	gs_texture_t *tex_a = gs_texture_create(W, H, GS_RGBA, 1, &da, 0);
	gs_texture_t *tex_b = gs_texture_create(W, H, GS_RGBA, 1, &db, 0);

	gs_enable_blending(false);
	gs_set_cull_mode(GS_NEITHER);

	/* STEP 1: Copy pass into RGBA texrender, read back */
	gs_texrender_t *rt = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), tex_a);
	draw_pass(rt, W, H, "Copy", tex_a);

	gs_stagesurf_t *st = gs_stagesurface_create(W, H, GS_RGBA);
	gs_stage_texture(st, gs_texrender_get_texture(rt));
	uint8_t *data;
	uint32_t linesize;
	if (!gs_stagesurface_map(st, &data, &linesize)) {
		fprintf(stderr, "map failed\n");
		return 4;
	}
	const uint8_t *c1 = data + (size_t)182 * linesize + 4 * 232; /* square centre */
	const uint8_t *c2 = data + (size_t)40 * linesize + 4 * 40;   /* background */
	printf("STEP1 Copy: centre=(%u,%u,%u,%u) bg=(%u,%u,%u,%u) [expect ~220 / ~25]\n", c1[0], c1[1],
	       c1[2], c1[3], c2[0], c2[1], c2[2], c2[3]);
	gs_stagesurface_unmap(st);

	/* STEP 2: Pack at 160x90, read back RG16F */
	gs_texrender_t *pk = gs_texrender_create(GS_RG16F, GS_ZS_NONE);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), tex_a);
	gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image2"), tex_b);
	set_texel("texel", W, H);
	draw_pass(pk, 160, 90, "Pack", tex_a);

	gs_stagesurf_t *st2 = gs_stagesurface_create(160, 90, GS_RG16F);
	gs_stage_texture(st2, gs_texrender_get_texture(pk));
	if (!gs_stagesurface_map(st2, &data, &linesize)) {
		fprintf(stderr, "map2 failed\n");
		return 5;
	}
	/* square centre in A: x=232 -> 58, y=182 -> 45 ; B centre x=248 -> 62 */
	const uint16_t *q1 = (const uint16_t *)(data + (size_t)45 * linesize) + 2 * 58;
	const uint16_t *q2 = (const uint16_t *)(data + (size_t)45 * linesize) + 2 * 40; /* bg */
	printf("STEP2 Pack: @A-centre lumA=%.3f lumB=%.3f | @bg lumA=%.3f lumB=%.3f [expect ~0.86/varies, bg ~0.1]\n",
	       h2f(q1[0]), h2f(q1[1]), h2f(q2[0]), h2f(q2[1]));
	gs_stagesurface_unmap(st2);

	printf("done\n");
	gs_leave_context();
	return 0;
}
