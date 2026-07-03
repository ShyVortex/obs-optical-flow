/*
 * optical-flow-filter.c — real-time optical-flow frame interpolation filter
 * for OBS Studio 32+.
 *
 * Concept: a video filter runs at canvas rate (e.g. 60 fps) while its target
 * source may update slower (e.g. 30 fps), so OBS renders duplicate frames.
 * This filter captures the target every tick (gpu-delay.c pattern), detects
 * NEW source frames via a tiny GPU difference map read back one tick later
 * (no pipeline stall), keeps the last two distinct frames A/B with their
 * arrival timestamps, and on duplicate ticks outputs a motion-compensated
 * in-between frame computed by pyramidal block-matching optical flow that
 * runs entirely in fragment shaders (see data/effects/optical-flow.effect).
 *
 * Latency: one source frame + one canvas tick (detection delay) by design.
 */

#include <obs-module.h>
#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#define S_FLOW_DIV "flow_div"
#define S_STRENGTH "strength"
#define S_OCCL "occlusion_sensitivity"
#define S_THRESH "detect_threshold"
#define S_LOW_LATENCY "low_latency"
#define S_DEBUG "debug_mode"

#define MAX_PYR 6
#define DIFF_W 48
#define DIFF_H 27
#define NS_PER_MS 1000000ULL

enum debug_mode {
	DEBUG_NONE = 0,
	DEBUG_FLOW = 1,
	DEBUG_SPLIT = 2,
};

struct pending_diff {
	bool valid;
	int slot;
	uint64_t ts;
};

struct of_filter {
	obs_source_t *context;
	gs_effect_t *effect;

	/* effect params */
	gs_eparam_t *ep_image;
	gs_eparam_t *ep_image2;
	gs_eparam_t *ep_flow_tex;
	gs_eparam_t *ep_prev_flow;
	gs_eparam_t *ep_temporal_flow;
	gs_eparam_t *ep_texel;
	gs_eparam_t *ep_flow_texel;
	gs_eparam_t *ep_frame_px;
	gs_eparam_t *ep_t_phase;
	gs_eparam_t *ep_has_prev;
	gs_eparam_t *ep_has_temporal;
	gs_eparam_t *ep_fine_pass;
	gs_eparam_t *ep_occl_k;
	gs_eparam_t *ep_strength;
	gs_eparam_t *ep_flow_quant;
	gs_eparam_t *ep_debug_mag;

	uint32_t cx, cy;
	bool target_valid;
	bool processed;
	bool need_reset;

	enum gs_color_space space;
	enum gs_color_format format;

	/* per-tick capture ring (novelty of a tick is only known one tick
	 * later, so the previous tick's pixels must stay alive) */
	gs_texrender_t *cap[3];
	uint64_t cap_ts[3];
	int cap_idx;

	/* last two distinct source frames */
	gs_texrender_t *frame_a, *frame_b;
	uint64_t ts_a, ts_b;
	int have_frames;
	bool pair_is_cut;

	/* new-frame detection */
	gs_texrender_t *diff_rt;
	gs_stagesurf_t *stage[2];
	struct pending_diff pending[2];
	int stage_next;

	/* optical flow resources */
	gs_texrender_t *pyr[MAX_PYR];      /* RG16F luma pyramid (A in R, B in G) */
	gs_texrender_t *flow_me[MAX_PYR];  /* RGBA16F raw flow per level */
	gs_texrender_t *flow_med[MAX_PYR]; /* RGBA16F median-filtered flow */
	gs_texrender_t *temporal;          /* previous pair's flow (seed) */
	uint32_t pyr_w[MAX_PYR], pyr_h[MAX_PYR];
	int levels;
	bool flow_valid;
	bool temporal_valid;

	gs_texrender_t *interp_rt; /* interpolation result (split view) */
	gs_texrender_t *out_rt;    /* per-tick output cache */
	bool out_valid;

	/* timing */
	uint64_t interval_canvas_ns;
	double src_interval_ns; /* EMA of source frame interval */

	/* settings */
	int flow_div;
	float strength;
	float occl_sens;
	float detect_thresh; /* 0..1 mean luma difference */
	bool low_latency;
	int debug;
};

/* ------------------------------------------------------------------ */

static const char *of_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("OpticalFlowFilter");
}

static void destroy_textures(struct of_filter *f)
{
	obs_enter_graphics();

	for (int i = 0; i < 3; i++) {
		gs_texrender_destroy(f->cap[i]);
		f->cap[i] = NULL;
		f->cap_ts[i] = 0;
	}
	gs_texrender_destroy(f->frame_a);
	gs_texrender_destroy(f->frame_b);
	f->frame_a = NULL;
	f->frame_b = NULL;

	gs_texrender_destroy(f->diff_rt);
	f->diff_rt = NULL;
	for (int i = 0; i < 2; i++) {
		gs_stagesurface_destroy(f->stage[i]);
		f->stage[i] = NULL;
		f->pending[i].valid = false;
	}

	for (int l = 0; l < MAX_PYR; l++) {
		gs_texrender_destroy(f->pyr[l]);
		gs_texrender_destroy(f->flow_me[l]);
		gs_texrender_destroy(f->flow_med[l]);
		f->pyr[l] = NULL;
		f->flow_me[l] = NULL;
		f->flow_med[l] = NULL;
	}
	gs_texrender_destroy(f->temporal);
	f->temporal = NULL;
	gs_texrender_destroy(f->interp_rt);
	f->interp_rt = NULL;
	gs_texrender_destroy(f->out_rt);
	f->out_rt = NULL;

	obs_leave_graphics();

	f->have_frames = 0;
	f->ts_a = 0;
	f->ts_b = 0;
	f->flow_valid = false;
	f->temporal_valid = false;
	f->out_valid = false;
	f->stage_next = 0;
	f->cap_idx = 0;
	f->src_interval_ns = 0.0;
	f->levels = 0;
	f->pair_is_cut = false;
}

/* Must be called inside the graphics context. */
static bool ensure_textures(struct of_filter *f)
{
	if (f->cap[0])
		return true;
	if (!f->cx || !f->cy)
		return false;

	for (int i = 0; i < 3; i++)
		f->cap[i] = gs_texrender_create(f->format, GS_ZS_NONE);
	f->interp_rt = gs_texrender_create(f->format, GS_ZS_NONE);
	f->out_rt = gs_texrender_create(f->format, GS_ZS_NONE);
	f->diff_rt = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	for (int i = 0; i < 2; i++) {
		f->stage[i] = gs_stagesurface_create(DIFF_W, DIFF_H, GS_RGBA);
		f->pending[i].valid = false;
	}

	uint32_t div = (uint32_t)(f->flow_div < 2 ? 2 : f->flow_div);
	uint32_t w = f->cx / div;
	uint32_t h = f->cy / div;
	if (w < 16)
		w = 16;
	if (h < 9)
		h = 9;

	int l = 0;
	while (l < MAX_PYR) {
		f->pyr_w[l] = w;
		f->pyr_h[l] = h;
		f->pyr[l] = gs_texrender_create(GS_RG16F, GS_ZS_NONE);
		f->flow_me[l] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
		f->flow_med[l] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
		l++;
		if (w / 2 < 24 || h / 2 < 14)
			break;
		w /= 2;
		h /= 2;
	}
	f->levels = l;
	f->temporal = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	f->temporal_valid = false;

	return true;
}

/* ------------------------------------------------------------------ */

static void of_update(void *data, obs_data_t *s)
{
	struct of_filter *f = data;

	int div = (int)obs_data_get_int(s, S_FLOW_DIV);
	bool lowlat = obs_data_get_bool(s, S_LOW_LATENCY);

	if (div != f->flow_div || lowlat != f->low_latency)
		f->need_reset = true;

	f->flow_div = div;
	f->low_latency = lowlat;
	f->strength = (float)obs_data_get_double(s, S_STRENGTH);
	f->occl_sens = (float)obs_data_get_double(s, S_OCCL);
	f->detect_thresh = (float)(obs_data_get_double(s, S_THRESH) / 100.0);
	f->debug = (int)obs_data_get_int(s, S_DEBUG);
}

static void of_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, S_FLOW_DIV, 4);
	obs_data_set_default_double(s, S_STRENGTH, 1.0);
	obs_data_set_default_double(s, S_OCCL, 0.5);
	obs_data_set_default_double(s, S_THRESH, 0.15);
	obs_data_set_default_bool(s, S_LOW_LATENCY, false);
	obs_data_set_default_int(s, S_DEBUG, DEBUG_NONE);
}

static obs_properties_t *of_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();

	obs_property_t *p = obs_properties_add_list(props, S_FLOW_DIV, obs_module_text("FlowScale"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("FlowScale.Half"), 2);
	obs_property_list_add_int(p, obs_module_text("FlowScale.Quarter"), 4);
	obs_property_list_add_int(p, obs_module_text("FlowScale.Eighth"), 8);

	obs_properties_add_float_slider(props, S_STRENGTH, obs_module_text("Strength"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, S_OCCL, obs_module_text("OcclSens"), 0.0, 1.0, 0.01);
	obs_properties_add_float_slider(props, S_THRESH, obs_module_text("DetectThresh"), 0.01, 2.0, 0.01);
	obs_properties_add_bool(props, S_LOW_LATENCY, obs_module_text("LowLatency"));

	p = obs_properties_add_list(props, S_DEBUG, obs_module_text("DebugMode"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("DebugMode.None"), DEBUG_NONE);
	obs_property_list_add_int(p, obs_module_text("DebugMode.Flow"), DEBUG_FLOW);
	obs_property_list_add_int(p, obs_module_text("DebugMode.Split"), DEBUG_SPLIT);

	return props;
}

static void *of_create(obs_data_t *settings, obs_source_t *context)
{
	struct of_filter *f = bzalloc(sizeof(*f));
	f->context = context;

	char *path = obs_module_file("effects/optical-flow.effect");
	if (path) {
		char *err = NULL;
		obs_enter_graphics();
		f->effect = gs_effect_create_from_file(path, &err);
		obs_leave_graphics();
		if (!f->effect)
			blog(LOG_ERROR, "[obs-optical-flow] failed to load effect: %s",
			     err ? err : "unknown error");
		bfree(err);
		bfree(path);
	} else {
		blog(LOG_ERROR, "[obs-optical-flow] effect file not found");
	}

	if (f->effect) {
		f->ep_image = gs_effect_get_param_by_name(f->effect, "image");
		f->ep_image2 = gs_effect_get_param_by_name(f->effect, "image2");
		f->ep_flow_tex = gs_effect_get_param_by_name(f->effect, "flow_tex");
		f->ep_prev_flow = gs_effect_get_param_by_name(f->effect, "prev_flow");
		f->ep_temporal_flow = gs_effect_get_param_by_name(f->effect, "temporal_flow");
		f->ep_texel = gs_effect_get_param_by_name(f->effect, "texel");
		f->ep_flow_texel = gs_effect_get_param_by_name(f->effect, "flow_texel");
		f->ep_frame_px = gs_effect_get_param_by_name(f->effect, "frame_px");
		f->ep_t_phase = gs_effect_get_param_by_name(f->effect, "t_phase");
		f->ep_has_prev = gs_effect_get_param_by_name(f->effect, "has_prev");
		f->ep_has_temporal = gs_effect_get_param_by_name(f->effect, "has_temporal");
		f->ep_fine_pass = gs_effect_get_param_by_name(f->effect, "fine_pass");
		f->ep_occl_k = gs_effect_get_param_by_name(f->effect, "occl_k");
		f->ep_strength = gs_effect_get_param_by_name(f->effect, "strength");
		f->ep_flow_quant = gs_effect_get_param_by_name(f->effect, "flow_quant");
		f->ep_debug_mag = gs_effect_get_param_by_name(f->effect, "debug_mag");
	}

	obs_source_update(context, settings);
	return f;
}

static void of_destroy(void *data)
{
	struct of_filter *f = data;

	destroy_textures(f);
	if (f->effect) {
		obs_enter_graphics();
		gs_effect_destroy(f->effect);
		obs_leave_graphics();
	}
	bfree(f);
}

/* ------------------------------------------------------------------ */

static bool check_size(struct of_filter *f)
{
	obs_source_t *target = obs_filter_get_target(f->context);

	f->target_valid = !!target;
	if (!f->target_valid)
		return true;

	uint32_t cx = obs_source_get_base_width(target);
	uint32_t cy = obs_source_get_base_height(target);

	f->target_valid = !!cx && !!cy;
	if (!f->target_valid)
		return true;

	if (cx != f->cx || cy != f->cy) {
		f->cx = cx;
		f->cy = cy;
		return true;
	}

	return false;
}

static void of_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	struct of_filter *f = data;

	f->processed = false;
	f->out_valid = false;

	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi) && ovi.fps_num > 0)
		f->interval_canvas_ns = util_mul_div64(ovi.fps_den, 1000000000ULL, ovi.fps_num);
	if (!f->interval_canvas_ns)
		f->interval_canvas_ns = 16666667;

	if (check_size(f) || f->need_reset) {
		destroy_textures(f);
		f->need_reset = false;
	}
}

/* ------------------------------------------------------------------ */
/* rendering helpers — all called inside the graphics context          */

static void draw_pass(struct of_filter *f, gs_texrender_t *dst, uint32_t w, uint32_t h, const char *tech,
		      gs_texture_t *sprite_tex)
{
	gs_texrender_reset(dst);
	if (!gs_texrender_begin(dst, w, h))
		return;

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

	while (gs_effect_loop(f->effect, tech))
		gs_draw_sprite(sprite_tex, 0, w, h);

	gs_texrender_end(dst);
}

static void draw_pass_space(struct of_filter *f, gs_texrender_t *dst, uint32_t w, uint32_t h,
			    enum gs_color_space space, const char *tech, gs_texture_t *sprite_tex)
{
	gs_texrender_reset(dst);
	if (!gs_texrender_begin_with_color_space(dst, w, h, space))
		return;

	struct vec4 clear;
	vec4_zero(&clear);
	gs_clear(GS_CLEAR_COLOR, &clear, 0.0f, 0);
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);

	while (gs_effect_loop(f->effect, tech))
		gs_draw_sprite(sprite_tex, 0, w, h);

	gs_texrender_end(dst);
}

static void set_texel(struct of_filter *f, gs_eparam_t *param, uint32_t w, uint32_t h)
{
	struct vec2 v;
	vec2_set(&v, 1.0f / (float)w, 1.0f / (float)h);
	gs_effect_set_vec2(param, &v);
}

static bool capture_target(struct of_filter *f, obs_source_t *target, obs_source_t *parent, int slot)
{
	gs_texrender_t *rt = f->cap[slot];

	if (gs_texrender_get_format(rt) != f->format) {
		gs_texrender_destroy(rt);
		rt = gs_texrender_create(f->format, GS_ZS_NONE);
		f->cap[slot] = rt;
	}

	gs_texrender_reset(rt);
	if (!gs_texrender_begin_with_color_space(rt, f->cx, f->cy, f->space))
		return false;

	uint32_t flags = obs_source_get_output_flags(target);
	bool custom_draw = (flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
	bool async = (flags & OBS_SOURCE_ASYNC) != 0;
	struct vec4 clear_color;

	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
	gs_ortho(0.0f, (float)f->cx, 0.0f, (float)f->cy, -100.0f, 100.0f);

	if (target == parent && !custom_draw && !async)
		obs_source_default_render(target);
	else
		obs_source_video_render(target);

	gs_texrender_end(rt);
	return true;
}

static void promote(struct of_filter *f, int slot, uint64_t ts, double mean_diff)
{
	/* frame interval statistics (before shifting timestamps) */
	bool big_gap = true;
	if (f->ts_b && ts > f->ts_b) {
		uint64_t gap = ts - f->ts_b;
		if (gap < 250 * NS_PER_MS) {
			big_gap = false;
			f->src_interval_ns = (f->src_interval_ns > 0.0)
						     ? f->src_interval_ns * 0.7 + (double)gap * 0.3
						     : (double)gap;
		}
	}

	gs_texrender_t *stolen = f->cap[slot];
	f->cap[slot] = f->frame_a ? f->frame_a : gs_texrender_create(f->format, GS_ZS_NONE);
	f->frame_a = f->frame_b;
	f->ts_a = f->ts_b;
	f->frame_b = stolen;
	f->ts_b = ts;

	if (f->have_frames < 2)
		f->have_frames++;

	f->flow_valid = false;
	f->pair_is_cut = (f->have_frames < 2) || big_gap || (mean_diff > 0.30);
}

static bool read_stage_diff(struct of_filter *f, int idx, double *mean, double *max_cell)
{
	uint8_t *data;
	uint32_t linesize;

	if (!gs_stagesurface_map(f->stage[idx], &data, &linesize))
		return false;

	uint64_t sum = 0;
	uint8_t maxv = 0;
	for (uint32_t y = 0; y < DIFF_H; y++) {
		const uint8_t *row = data + (size_t)y * linesize;
		for (uint32_t x = 0; x < DIFF_W; x++) {
			uint8_t v = row[x * 4];
			sum += v;
			if (v > maxv)
				maxv = v;
		}
	}
	gs_stagesurface_unmap(f->stage[idx]);

	*mean = (double)sum / (255.0 * DIFF_W * DIFF_H);
	*max_cell = (double)maxv / 255.0;
	return true;
}

static void maybe_read_and_promote(struct of_filter *f, int idx)
{
	if (!f->pending[idx].valid)
		return;

	int slot = f->pending[idx].slot;
	uint64_t ts = f->pending[idx].ts;
	f->pending[idx].valid = false;

	double mean, max_cell;
	if (!read_stage_diff(f, idx, &mean, &max_cell))
		return;

	/* global-mean rule catches full-frame changes; the max-cell rule
	 * catches a small moving object (e.g. a logo) whose difference is
	 * diluted in the global mean */
	if (mean > (double)f->detect_thresh || max_cell > 0.05)
		promote(f, slot, ts, mean);
}

static void schedule_diff(struct of_filter *f, int slot, uint64_t ts)
{
	gs_texture_t *cur = gs_texrender_get_texture(f->cap[slot]);
	gs_texture_t *ref = gs_texrender_get_texture(f->frame_b);
	if (!cur || !ref)
		return;

	gs_effect_set_texture(f->ep_image, cur);
	gs_effect_set_texture(f->ep_image2, ref);
	/* taps at +- a quarter of the diff-cell pitch: spreads the 4 samples
	 * across the whole cell so small moving objects are seen */
	struct vec2 v;
	vec2_set(&v, 1.0f / (4.0f * DIFF_W), 1.0f / (4.0f * DIFF_H));
	gs_effect_set_vec2(f->ep_texel, &v);
	draw_pass(f, f->diff_rt, DIFF_W, DIFF_H, "Diff", cur);

	gs_texture_t *diff_tex = gs_texrender_get_texture(f->diff_rt);
	if (!diff_tex)
		return;

	gs_stage_texture(f->stage[f->stage_next], diff_tex);
	f->pending[f->stage_next].valid = true;
	f->pending[f->stage_next].slot = slot;
	f->pending[f->stage_next].ts = ts;
	f->stage_next ^= 1;
}

static void compute_flow(struct of_filter *f)
{
	gs_texture_t *tex_a = gs_texrender_get_texture(f->frame_a);
	gs_texture_t *tex_b = gs_texrender_get_texture(f->frame_b);
	if (!tex_a || !tex_b)
		return;

	/* pyramid level 0: packed luma. Tap spread scales with the
	 * downsample factor so the box prefilter covers the cell (avoids
	 * aliasing at 1/8 flow resolution). */
	gs_effect_set_texture(f->ep_image, tex_a);
	gs_effect_set_texture(f->ep_image2, tex_b);
	{
		float spread = (f->flow_div >= 8) ? 2.0f : 1.0f;
		struct vec2 v;
		vec2_set(&v, spread / (float)f->cx, spread / (float)f->cy);
		gs_effect_set_vec2(f->ep_texel, &v);
	}
	draw_pass(f, f->pyr[0], f->pyr_w[0], f->pyr_h[0], "Pack", tex_a);

	/* downsample chain */
	for (int l = 1; l < f->levels; l++) {
		gs_texture_t *src = gs_texrender_get_texture(f->pyr[l - 1]);
		gs_effect_set_texture(f->ep_image, src);
		set_texel(f, f->ep_texel, f->pyr_w[l - 1], f->pyr_h[l - 1]);
		draw_pass(f, f->pyr[l], f->pyr_w[l], f->pyr_h[l], "Down", src);
	}

	/* motion estimation, coarse to fine, median after each level */
	for (int l = f->levels - 1; l >= 0; l--) {
		gs_texture_t *lvl = gs_texrender_get_texture(f->pyr[l]);
		bool has_prev = l < f->levels - 1;
		gs_texture_t *prev = has_prev ? gs_texrender_get_texture(f->flow_med[l + 1]) : lvl;
		bool has_temporal = !has_prev && f->temporal_valid;
		gs_texture_t *temporal = has_temporal ? gs_texrender_get_texture(f->temporal) : lvl;

		gs_effect_set_texture(f->ep_image, lvl);
		gs_effect_set_texture(f->ep_prev_flow, prev);
		gs_effect_set_texture(f->ep_temporal_flow, temporal);
		set_texel(f, f->ep_texel, f->pyr_w[l], f->pyr_h[l]);
		set_texel(f, f->ep_flow_texel, f->pyr_w[has_prev ? l + 1 : l], f->pyr_h[has_prev ? l + 1 : l]);
		gs_effect_set_float(f->ep_has_prev, has_prev ? 1.0f : 0.0f);
		gs_effect_set_float(f->ep_has_temporal, has_temporal ? 1.0f : 0.0f);
		gs_effect_set_float(f->ep_fine_pass, (l == 0) ? 1.0f : 0.0f);
		draw_pass(f, f->flow_me[l], f->pyr_w[l], f->pyr_h[l], "Motion", lvl);

		gs_texture_t *raw = gs_texrender_get_texture(f->flow_me[l]);
		gs_effect_set_texture(f->ep_image, raw);
		set_texel(f, f->ep_texel, f->pyr_w[l], f->pyr_h[l]);
		draw_pass(f, f->flow_med[l], f->pyr_w[l], f->pyr_h[l], "SmoothFlow", raw);
	}

	/* keep as temporal seed for the next pair */
	gs_texture_t *fin = gs_texrender_get_texture(f->flow_med[0]);
	if (fin) {
		gs_effect_set_texture(f->ep_image, fin);
		draw_pass(f, f->temporal, f->pyr_w[0], f->pyr_h[0], "Copy", fin);
		f->temporal_valid = true;
	}

	f->flow_valid = true;
}

static void compose_copy(struct of_filter *f, gs_texture_t *tex)
{
	if (!tex)
		return;
	gs_effect_set_texture(f->ep_image, tex);
	draw_pass_space(f, f->out_rt, f->cx, f->cy, f->space, "Copy", tex);
	f->out_valid = true;
}

static void set_interp_params(struct of_filter *f, gs_texture_t *tex_a, gs_texture_t *tex_b, float t)
{
	struct vec2 px;

	gs_effect_set_texture(f->ep_image, tex_a);
	gs_effect_set_texture(f->ep_image2, tex_b);
	gs_effect_set_texture(f->ep_flow_tex, gs_texrender_get_texture(f->flow_med[0]));
	vec2_set(&px, (float)f->cx, (float)f->cy);
	gs_effect_set_vec2(f->ep_frame_px, &px);
	gs_effect_set_float(f->ep_t_phase, t);
	gs_effect_set_float(f->ep_occl_k, 2.0f + f->occl_sens * 10.0f);
	gs_effect_set_float(f->ep_strength, f->strength);
	/* half a flow texel in source px: independent fwd/bwd rounding may
	 * disagree by this much without indicating occlusion */
	gs_effect_set_float(f->ep_flow_quant, 0.5f * (float)f->cx / (float)f->pyr_w[0]);
}

static void compose_interp(struct of_filter *f, float t)
{
	gs_texture_t *tex_a = gs_texrender_get_texture(f->frame_a);
	gs_texture_t *tex_b = gs_texrender_get_texture(f->frame_b);
	if (!tex_a || !tex_b)
		return;

	if (!f->flow_valid)
		compute_flow(f);
	if (!f->flow_valid)
		return;

	if (f->debug == DEBUG_FLOW) {
		struct vec2 px;
		gs_effect_set_texture(f->ep_image, tex_b);
		gs_effect_set_texture(f->ep_flow_tex, gs_texrender_get_texture(f->flow_med[0]));
		vec2_set(&px, (float)f->cx, (float)f->cy);
		gs_effect_set_vec2(f->ep_frame_px, &px);
		gs_effect_set_float(f->ep_debug_mag, 1.0f);
		draw_pass_space(f, f->out_rt, f->cx, f->cy, f->space, "DebugFlow", tex_b);
		f->out_valid = true;
		return;
	}

	if (f->debug == DEBUG_SPLIT) {
		set_interp_params(f, tex_a, tex_b, t);
		draw_pass(f, f->interp_rt, f->cx, f->cy, "Interp", tex_a);

		gs_texture_t *interp = gs_texrender_get_texture(f->interp_rt);
		gs_texture_t *nearest = (t < 0.5f) ? tex_a : tex_b;
		if (!interp)
			return;
		gs_effect_set_texture(f->ep_image, interp);
		gs_effect_set_texture(f->ep_image2, nearest);
		draw_pass_space(f, f->out_rt, f->cx, f->cy, f->space, "Split", interp);
		f->out_valid = true;
		return;
	}

	set_interp_params(f, tex_a, tex_b, t);
	draw_pass_space(f, f->out_rt, f->cx, f->cy, f->space, "Interp", tex_a);
	f->out_valid = true;
}

/* ------------------------------------------------------------------ */
/* final output draw — gpu-delay.c pattern for color space handling    */

static const char *get_tech_name_and_multiplier(enum gs_color_space current_space,
						enum gs_color_space source_space, float *multiplier)
{
	const char *tech_name = "Draw";
	*multiplier = 1.f;

	switch (source_space) {
	case GS_CS_SRGB:
	case GS_CS_SRGB_16F:
		if (current_space == GS_CS_709_SCRGB) {
			tech_name = "DrawMultiply";
			*multiplier = obs_get_video_sdr_white_level() / 80.0f;
		}
		break;
	case GS_CS_709_EXTENDED:
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
			tech_name = "DrawTonemap";
			break;
		case GS_CS_709_SCRGB:
			tech_name = "DrawMultiply";
			*multiplier = obs_get_video_sdr_white_level() / 80.0f;
			break;
		default:
			break;
		}
		break;
	case GS_CS_709_SCRGB:
		switch (current_space) {
		case GS_CS_SRGB:
		case GS_CS_SRGB_16F:
			tech_name = "DrawMultiplyTonemap";
			*multiplier = 80.0f / obs_get_video_sdr_white_level();
			break;
		case GS_CS_709_EXTENDED:
			tech_name = "DrawMultiply";
			*multiplier = 80.0f / obs_get_video_sdr_white_level();
			break;
		default:
			break;
		}
	}

	return tech_name;
}

static void draw_output(struct of_filter *f)
{
	if (!f->out_valid)
		return;

	gs_texture_t *tex = gs_texrender_get_texture(f->out_rt);
	if (!tex)
		return;

	const enum gs_color_space current_space = gs_get_color_space();
	float multiplier;
	const char *technique = get_tech_name_and_multiplier(current_space, f->space, &multiplier);

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(effect, "image"), tex);
	gs_effect_set_float(gs_effect_get_param_by_name(effect, "multiplier"), multiplier);

	while (gs_effect_loop(effect, technique))
		gs_draw_sprite(tex, 0, f->cx, f->cy);

	gs_enable_framebuffer_srgb(previous);
}

/* ------------------------------------------------------------------ */

static void of_render(void *data, gs_effect_t *unused_effect)
{
	UNUSED_PARAMETER(unused_effect);
	struct of_filter *f = data;

	if (f->processed) {
		draw_output(f);
		return;
	}

	obs_source_t *target = obs_filter_get_target(f->context);
	obs_source_t *parent = obs_filter_get_parent(f->context);

	if (!f->effect || !f->target_valid || !target || !parent) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	const enum gs_color_space preferred_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};
	const enum gs_color_space space =
		obs_source_get_color_space(target, OBS_COUNTOF(preferred_spaces), preferred_spaces);
	const enum gs_color_format format = gs_get_format_from_space(space);

	if (format != f->format) {
		destroy_textures(f);
		f->format = format;
	}
	f->space = space;

	if (!ensure_textures(f)) {
		obs_source_skip_video_filter(f->context);
		return;
	}

	f->processed = true;

	const uint64_t now = os_gettime_ns();

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	const bool prev_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);

	/* 1. capture this tick */
	f->cap_idx = (f->cap_idx + 1) % 3;
	const int ci = f->cap_idx;
	bool captured = capture_target(f, target, parent, ci);
	f->cap_ts[ci] = captured ? now : 0;

	/* 2. new-frame detection */
	if (captured) {
		if (!f->frame_b) {
			promote(f, ci, now, 1.0);
		} else {
			if (!f->low_latency)
				maybe_read_and_promote(f, f->stage_next ^ 1);
			schedule_diff(f, ci, now);
			if (f->low_latency)
				maybe_read_and_promote(f, f->stage_next ^ 1);
		}
	}

	/* 3. presentation decision */
	const uint64_t canvas_ns = f->interval_canvas_ns;
	const bool bypass = f->src_interval_ns > 0.0 && f->src_interval_ns < (double)canvas_ns * 1.35;

	/* the freshest pixels of this tick: frame_b if it was promoted just
	 * now, else this tick's capture — but only if the capture succeeded
	 * (a stale ring slot must not masquerade as the current image) */
	gs_texture_t *cur_tex = NULL;
	if (f->frame_b && f->ts_b == now)
		cur_tex = gs_texrender_get_texture(f->frame_b);
	else if (captured)
		cur_tex = gs_texrender_get_texture(f->cap[ci]);

	if (bypass || f->have_frames == 0) {
		if (!cur_tex && f->frame_b)
			cur_tex = gs_texrender_get_texture(f->frame_b);
		compose_copy(f, cur_tex);
	} else if (f->have_frames == 1) {
		compose_copy(f, gs_texrender_get_texture(f->frame_b));
	} else {
		/* Presentation lag: exactly one source interval plus the
		 * one-tick detection delay. A pair (A,B) becomes known one
		 * tick after B arrives and is replaced one interval later;
		 * this lag makes "present" sweep tsA..tsB across exactly
		 * that window (cadence jitter is absorbed by the clamps
		 * below). */
		uint64_t delay;
		if (f->src_interval_ns > 0.0) {
			delay = (uint64_t)f->src_interval_ns;
			if (!f->low_latency)
				delay += canvas_ns;
		} else {
			delay = canvas_ns * 3;
		}
		if (delay > 250 * NS_PER_MS)
			delay = 250 * NS_PER_MS;

		const uint64_t present = (now > delay) ? now - delay : 0;

		if (present >= f->ts_b || f->ts_b <= f->ts_a) {
			compose_copy(f, gs_texrender_get_texture(f->frame_b));
		} else if (present <= f->ts_a) {
			compose_copy(f, gs_texrender_get_texture(f->frame_a));
		} else {
			const float t =
				(float)((double)(present - f->ts_a) / (double)(f->ts_b - f->ts_a));
			if (f->pair_is_cut || f->strength <= 0.001f) {
				compose_copy(f, gs_texrender_get_texture(t < 0.5f ? f->frame_a : f->frame_b));
			} else if (t < 0.04f) {
				compose_copy(f, gs_texrender_get_texture(f->frame_a));
			} else if (t > 0.96f) {
				compose_copy(f, gs_texrender_get_texture(f->frame_b));
			} else {
				compose_interp(f, t);
				if (!f->out_valid)
					compose_copy(f, gs_texrender_get_texture(f->frame_b));
			}
		}
	}

	gs_enable_framebuffer_srgb(prev_srgb);
	gs_blend_state_pop();

	draw_output(f);
}

static enum gs_color_space of_get_color_space(void *data, size_t count,
					      const enum gs_color_space *preferred_spaces)
{
	struct of_filter *f = data;

	enum gs_color_space space = f->space;
	for (size_t i = 0; i < count; i++) {
		if (preferred_spaces[i] == f->space)
			return f->space;
	}
	return count ? preferred_spaces[0] : space;
}

struct obs_source_info optical_flow_filter_info = {
	.id = "optical_flow_interpolation_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = of_get_name,
	.create = of_create,
	.destroy = of_destroy,
	.update = of_update,
	.get_defaults = of_defaults,
	.get_properties = of_properties,
	.video_tick = of_tick,
	.video_render = of_render,
	.video_get_color_space = of_get_color_space,
};
