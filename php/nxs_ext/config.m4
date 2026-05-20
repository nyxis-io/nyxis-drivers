PHP_ARG_ENABLE([nxs],
  [whether to enable NXS reader extension],
  [AS_HELP_STRING([--enable-nxs], [Enable NXS binary reader extension])])

if test "$PHP_NXS" != "no"; then
  PHP_NEW_EXTENSION(nxs, nxs_ext.c, $ext_shared)
fi
