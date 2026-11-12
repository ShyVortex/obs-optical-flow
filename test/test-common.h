/* Shared setup for the standalone libobs test harnesses.
 *
 * Creates a real libobs graphics device (the same effect parser, shader
 * transpiler and GPU driver OBS uses) without starting OBS itself.
 *
 * Common command line for every harness:
 *   --backend opengl|d3d11   graphics module (default: d3d11 on Windows,
 *                            opengl elsewhere)
 *   --adapter N              GPU index (Windows/D3D11: 0 is often the iGPU
 *                            on hybrid laptops; ignored by OpenGL)
 *   --effect PATH            effect file (default: data/effects/optical-flow.effect,
 *                            relative to the working directory)
 * A bare first argument is also accepted as the effect path.
 *
 * Linux needs a display connection registered with libobs before the device
 * is created: Wayland if WAYLAND_DISPLAY is set, X11 (EGL) otherwise. The
 * client libraries are loaded at runtime, so the tests don't link them.
 *
 * macOS: OBS_GRAPHICS_MODULE may hold the full path of the graphics module;
 * the default is the one inside /Applications/OBS.app.
 */
#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <graphics/graphics.h>
#include <util/platform.h>

#if !defined(_WIN32) && !defined(__APPLE__)
#include <dlfcn.h>
#include <obs-nix-platform.h>
#endif

struct test_opts {
	const char *backend;
	const char *effect;
	uint32_t adapter;
};

static inline void test_usage(const char *prog)
{
	fprintf(stderr, "usage: %s [--backend opengl|d3d11] [--adapter N] [--effect PATH | PATH]\n", prog);
}

static inline struct test_opts test_parse_args(int argc, char **argv)
{
	struct test_opts o;
#ifdef _WIN32
	o.backend = "d3d11";
#else
	o.backend = "opengl";
#endif
	o.effect = "data/effects/optical-flow.effect";
	o.adapter = 0;

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--backend") && i + 1 < argc) {
			o.backend = argv[++i];
		} else if (!strcmp(argv[i], "--adapter") && i + 1 < argc) {
			o.adapter = (uint32_t)strtoul(argv[++i], NULL, 10);
		} else if (!strcmp(argv[i], "--effect") && i + 1 < argc) {
			o.effect = argv[++i];
		} else if (argv[i][0] != '-') {
			o.effect = argv[i];
		} else {
			test_usage(argv[0]);
			exit(64);
		}
	}
	return o;
}

#if !defined(_WIN32) && !defined(__APPLE__)
/* Register a Wayland or X11 display with libobs (required by libobs-opengl). */
static inline bool test_setup_nix_display(void)
{
	if (getenv("WAYLAND_DISPLAY")) {
		void *lib = dlopen("libwayland-client.so.0", RTLD_NOW);
		void *(*connect)(const char *) = lib ? (void *(*)(const char *))dlsym(lib, "wl_display_connect")
						     : NULL;
		void *dpy = connect ? connect(NULL) : NULL;
		if (dpy) {
			obs_set_nix_platform(OBS_NIX_PLATFORM_WAYLAND);
			obs_set_nix_platform_display(dpy);
			printf("display: Wayland\n");
			return true;
		}
		fprintf(stderr, "Wayland connection failed, trying X11\n");
	}

	void *lib = dlopen("libX11.so.6", RTLD_NOW);
	void *(*open_display)(const char *) = lib ? (void *(*)(const char *))dlsym(lib, "XOpenDisplay") : NULL;
	void *dpy = open_display ? open_display(NULL) : NULL;
	if (!dpy) {
		fprintf(stderr, "no Wayland or X11 display available (run inside a desktop session or xvfb-run)\n");
		return false;
	}
	obs_set_nix_platform(OBS_NIX_PLATFORM_X11_EGL);
	obs_set_nix_platform_display(dpy);
	printf("display: X11 (EGL)\n");
	return true;
}
#endif

/* Create the graphics device and enter its context; exits on failure. */
static inline graphics_t *test_gs_create(const struct test_opts *o)
{
	const char *module;
	if (!strcmp(o->backend, "d3d11")) {
#ifndef _WIN32
		fprintf(stderr, "the d3d11 backend is only available on Windows\n");
		exit(2);
#endif
		module = "libobs-d3d11";
	} else if (!strcmp(o->backend, "opengl")) {
#ifdef __APPLE__
		module = getenv("OBS_GRAPHICS_MODULE")
				 ? getenv("OBS_GRAPHICS_MODULE")
				 : "/Applications/OBS.app/Contents/Frameworks/libobs-opengl.dylib";
#else
		module = "libobs-opengl";
#endif
	} else {
		fprintf(stderr, "unknown backend '%s' (expected opengl or d3d11)\n", o->backend);
		exit(64);
	}

#if !defined(_WIN32) && !defined(__APPLE__)
	if (!test_setup_nix_display())
		exit(2);
#endif

	graphics_t *graphics = NULL;
	int ret = gs_create(&graphics, module, o->adapter);
	if (ret != GS_SUCCESS) {
		fprintf(stderr, "gs_create(%s, adapter %u) failed: %d\n", module, o->adapter, ret);
		exit(2);
	}
	gs_enter_context(graphics);
	printf("graphics: %s (module %s, adapter %u)\n", gs_get_device_name(), module, o->adapter);
	return graphics;
}

static inline gs_effect_t *test_load_effect(const char *path)
{
	char *err = NULL;
	uint64_t t0 = os_gettime_ns();
	gs_effect_t *effect = gs_effect_create_from_file(path, &err);
	if (!effect) {
		fprintf(stderr, "EFFECT COMPILE FAILED (%s):\n%s\n", path, err ? err : "(no error string)");
		exit(3);
	}
	printf("effect compiled in %.1f ms\n", (double)(os_gettime_ns() - t0) / 1e6);
	return effect;
}

static inline double test_now_ms(void)
{
	return (double)os_gettime_ns() / 1e6;
}
