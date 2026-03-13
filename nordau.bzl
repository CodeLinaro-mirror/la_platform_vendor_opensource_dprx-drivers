load(":dprx_modules.bzl", "dprx_driver_modules")
load(":dprx_driver_build.bzl", "define_target_variant_modules")
load("//soc-repo:target_variants.bzl", "get_all_la_variants")

def define_nordau():
    for (t, v) in get_all_la_variants():
        if t == "autogvm":
            define_target_variant_modules(
		target = t,
		variant = v,
		registry = dprx_driver_modules,
		modules = [
		    "dprx",
		],
		config_options = [
		    "CONFIG_DPRX",
	    ],
	)
