files_pvr = files(
  'mesa_context.c',
  'pvrdrawable.c',
  'pvrdri.c',
  'pvrdri.h',
  'pvrext.c',
  'pvrimage.c',
  'pvrimage.h',
  'pvrmesa.h',
  'pvrutil.c',
  'queue.h'
)

libpvr = static_library(
  'pvr',
  files_pvr,
  include_directories : [
  inc_include, inc_src, inc_mapi, inc_mapi_glapi, inc_mesa, inc_mesa_main, inc_gallium, inc_gallium_aux, inc_dri_common, inc_d3d, inc_d3d9, inc_gl, inc_gl_int, inc_uapi,
  ],
)
