project('lokdocview4', ['c', 'cpp'],
  version: '0.1.0',
  license: 'MPL-2.0',
  meson_version: '>= 1.0.0',
  default_options: [
    'c_std=c11',
    'cpp_std=c++20',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

gtk_dep = dependency('gtk4', version: '>= 4.12')
json_dep = dependency('json-glib-1.0')
dl_dep = meson.get_compiler('c').find_library('dl', required: false)

# Default LibreOffice program dir, derived like LibreOffice's own build does
# ($(LIBDIR)/libreoffice/program). Only a default: the widget resolves the
# actual install at runtime (see getDefaultLOPath() in src/lokdocview.cxx).
lo_path = get_option('lo_path')
if lo_path == ''
  lo_path = get_option('prefix') / get_option('libdir') / 'libreoffice' / 'program'
endif

conf = configuration_data()
conf.set_quoted('LOK_PATH', lo_path)
configure_file(output: 'config.h', configuration: conf)

inc = include_directories('include')

lokdocview_lib = shared_library('lokdocview4',
  'src/lokdocview.cxx',
  'src/tilebuffer.cxx',
  include_directories: inc,
  dependencies: [gtk_dep, json_dep, dl_dep],
  cpp_args: [
    '-DG_LOG_DOMAIN="LOKDocView"',
  ],
  gnu_symbol_visibility: 'hidden',
  version: meson.project_version(),
  install: true,
)

lokdocview_dep = declare_dependency(
  link_with: lokdocview_lib,
  include_directories: inc,
  dependencies: gtk_dep,
)

install_headers(
  'include/LibreOfficeKit/LibreOfficeKitGtk.h',
  'include/LibreOfficeKit/LibreOfficeKit.h',
  'include/LibreOfficeKit/LibreOfficeKitTypes.h',
  subdir: 'lokdocview4/LibreOfficeKit',
)

import('pkgconfig').generate(lokdocview_lib,
  name: 'lokdocview4',
  description: 'GTK4 widget for viewing and editing documents with LibreOfficeKit',
  requires: ['gtk4'],
  subdirs: 'lokdocview4',
)

if get_option('demo')
  executable('lokdocview4-demo',
    'demo/main.c',
    dependencies: lokdocview_dep,
    c_args: ['-DG_LOG_DOMAIN="LOKDocViewDemo"'],
  )
endif
