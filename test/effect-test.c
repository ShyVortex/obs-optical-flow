/* Standalone check: create a real libobs graphics context and compile an
 * .effect file through OBS' effect parser + shader transpiler.
 * Usage: effect-test <path-to-effect> [graphics-module-path] */

#include <stdio.h>
#include <graphics/graphics.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <effect-file> [graphics-module]\n", argv[0]);
		return 1;
	}

	const char *module = (argc > 2) ? argv[2]
					: "/Applications/OBS.app/Contents/Frameworks/libobs-opengl.dylib";

	graphics_t *graphics = NULL;
	int ret = gs_create(&graphics, module, 0);
	if (ret != GS_SUCCESS) {
		fprintf(stderr, "gs_create failed: %d\n", ret);
		return 2;
	}

	gs_enter_context(graphics);

	char *err = NULL;
	gs_effect_t *effect = gs_effect_create_from_file(argv[1], &err);
	if (!effect) {
		fprintf(stderr, "EFFECT COMPILE FAILED:\n%s\n", err ? err : "(no error string)");
		gs_leave_context();
		gs_destroy(graphics);
		return 3;
	}

	printf("EFFECT OK: all techniques compiled\n");

	gs_effect_destroy(effect);
	gs_leave_context();
	gs_destroy(graphics);
	return 0;
}
