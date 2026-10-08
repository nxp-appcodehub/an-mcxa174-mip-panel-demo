# config to select component, the format is CONFIG_USE_${component}
# This standalone (cmake-freestanding) project embeds the required MCUXpresso
# SDK sources directly, so component selection is expressed statically here and
# the concrete file list lives in CMakeLists.txt.

set(CONFIG_COMPILER gcc)
set(CONFIG_TOOLCHAIN armgcc)
set(CONFIG_USE_COMPONENT_CONFIGURATION false)
set(CONFIG_CORE cm33)
set(CONFIG_DEVICE MCXA174)
set(CONFIG_BOARD frdmmcxa174)
set(CONFIG_KIT frdmmcxa174)
set(CONFIG_DEVICE_ID MCXA174)
set(CONFIG_FPU SP_FPU)
set(CONFIG_DSP '')
set(CONFIG_CORE_ID cm33_core0)
