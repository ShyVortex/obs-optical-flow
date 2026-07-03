/*
 * obs-optical-flow — real-time optical-flow frame interpolation for OBS Studio.
 */

#include <obs-module.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-optical-flow", "en-US")

extern struct obs_source_info optical_flow_filter_info;

bool obs_module_load(void)
{
	obs_register_source(&optical_flow_filter_info);
	blog(LOG_INFO, "[obs-optical-flow] plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return "Real-time optical flow frame interpolation filter";
}

MODULE_EXPORT const char *obs_module_name(void)
{
	return "Optical Flow Frame Interpolation";
}
