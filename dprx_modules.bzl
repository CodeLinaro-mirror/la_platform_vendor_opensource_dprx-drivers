load(":dprx_driver_build.bzl", "dprx_module_entry")

dprx_driver_modules = dprx_module_entry([":dprx_drivers_headers"])
module_entry = dprx_driver_modules.register

#---------- MSM-DRM MODULE -------------------------

module_entry(
      name = "dprx",
      srcs = [
           "virtio_dprx.c",
           "virtio_dprx_virtq.c",
           "virtio_media_ioctl.c",
	   "virtio_dprx_debugfs.c"
	   ],
)

