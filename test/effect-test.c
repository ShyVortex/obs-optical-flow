/* Standalone check: create a real libobs graphics context and compile an
 * .effect file through OBS' effect parser + shader transpiler (GLSL on
 * OpenGL, HLSL on Direct3D 11).
 * Usage: effect-test [--backend opengl|d3d11] [--adapter N] [--effect PATH | PATH] */

#include "test-common.h"

int main(int argc, char **argv)
{
	struct test_opts o = test_parse_args(argc, argv);
	graphics_t *graphics = test_gs_create(&o);
	gs_effect_t *effect = test_load_effect(o.effect);

	printf("EFFECT OK: all techniques compiled\n");

	gs_effect_destroy(effect);
	gs_leave_context();
	gs_destroy(graphics);
	return 0;
}
