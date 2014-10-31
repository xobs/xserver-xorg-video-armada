#  Copyright 2005 Adam Jackson.
#
#  Permission is hereby granted, free of charge, to any person obtaining a
#  copy of this software and associated documentation files (the "Software"),
#  to deal in the Software without restriction, including without limitation
#  on the rights to use, copy, modify, merge, publish, distribute, sub
#  license, and/or sell copies of the Software, and to permit persons to whom
#  the Software is furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice (including the next
#  paragraph) shall be included in all copies or substantial portions of the
#  Software.
#
#  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
#  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
#  FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.  IN NO EVENT SHALL
#  ADAM JACKSON BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
#  IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
#  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#
# Process this file with autoconf to produce a configure script

AC_PREREQ(2.57)
AC_INIT([xf86-video-armada],
	[0.0.0],
	[],
	[xf86-video-armada])

DRIVER_NAME=armada

AC_CONFIG_SRCDIR([Makefile.am])
AC_CONFIG_HEADER([config.h])
AC_CONFIG_AUX_DIR(build-aux)
AC_CONFIG_MACRO_DIR([m4])

AM_INIT_AUTOMAKE([foreign dist-bzip2])
AM_MAINTAINER_MODE

# Require X.Org macros 1.8 or later for MAN_SUBSTS set by XORG_MANPAGE_SECTIONS
m4_ifndef([XORG_MACROS_VERSION],
          [m4_fatal([must install xorg-macros 1.8 or later before running autoconf/autogen.
  Hint: either install from source, git://anongit.freedesktop.org/xorg/util/macros or,
  depending on you distribution, try package 'xutils-dev' or 'xorg-x11-util-macros'])])

XORG_MACROS_VERSION(1.8)
XORG_DEFAULT_OPTIONS

# Require X.Org server macros (i.e. XORG_DRIVER_CHECK_EXT) to check fori
# required modules 
m4_ifndef([XORG_DRIVER_CHECK_EXT],
          [m4_fatal([must install xorg-server macros before running autoconf/autogen.
  Hint: either install from source, git://anongit.freedesktop.org/xorg/xserver or,
  depending on your distribution, try package 'xserver-xorg-dev' or 'xorg-x11-server-devel'])])

# Checks for programs.
AC_DISABLE_STATIC
AC_PROG_LIBTOOL
AC_PROG_CC

PKG_CHECK_MODULES(UDEV, [libudev], [udev=yes], [udev=no])
if test x"$udev" = xyes; then
   AC_DEFINE(HAVE_UDEV,1,[Enable udev-based monitor hotplug detection])
fi


AH_TOP([#include "xorg-server.h"])

AC_ARG_WITH(xorg-module-dir,
            AC_HELP_STRING([--with-xorg-module-dir=DIR],
                           [Default xorg module directory [[default=$libdir/xorg/modules]]]),
            [moduledir="$withval"],
            [moduledir="$libdir/xorg/modules"])

# Vivante GPU acceleration using libGAL
AC_ARG_ENABLE(vivante,
	      AC_HELP_STRING([--disable-vivante],
			     [Disable Vivante libGAL acceleration support [[default=enable]]]),
	      [ACCEL_GALCORE="$enableval"],
	      [ACCEL_GALCORE=auto])

AC_ARG_WITH(libgal-include,
            AC_HELP_STRING([--with-libgal-include=PATH],
                           [specify directory for installed libGAL include files [[default=/usr/local/include/HAL]]]),
            [libgal_include="$withval"],
            [libgal_include="/usr/local/include/HAL"])

AC_ARG_WITH(libgal-lib,
            AC_HELP_STRING([--with-libgal-lib=PATH],
                           [specify directory for installed libGAL library [[default=/usr/local/lib]]]),
            [libgal_lib="$withval"],
            [libgal_lib="/usr/local/lib"])

AS_IF([test x$ACCEL_GALCORE != xno],
      [
   LIBGAL_CFLAGS="-I$libgal_include"
   LIBGAL_LIBS="-L$libgal_lib -lGAL"

   saved_CPPFLAGS="$CPPFLAGS"
   CPPFLAGS="$CPPFLAGS $LIBGAL_CFLAGS"
   AC_CHECK_HEADERS([gc_hal.h gc_hal_base.h gc_hal_enum.h],
                    [],
                    AS_IF([test x$ACCEL_GALCORE = xyes],
                          [AC_MSG_ERROR([required libGAL header file missing])],
			  [ACCEL_GALCORE=no]))
   AC_CHECK_HEADERS([gc_hal_raster.h],
                    [],
                    AS_IF([test x$ACCEL_GALCORE = xyes],
                          [AC_MSG_ERROR([required libGAL header file missing])],
			  [ACCEL_GALCORE=no]),
                    [#include <gc_hal.h>])
   CPPFLAGS="$saved_CPPFLAGS"
   AS_IF([test x$ACCEL_GALCORE = xauto], [ACCEL_GALCORE=yes])
   AS_IF([test x$ACCEL_GALCORE = xyes], [DRIVERS="vivante_gpu $DRIVERS"])
])
AC_MSG_CHECKING([whether to build Vivante libGAL acceleration support])
AS_IF([test x$ACCEL_GALCORE = xyes],
      [AC_DEFINE(HAVE_ACCEL_GALCORE,1,[Enable Vivante libGAL acceleration support])])
AM_CONDITIONAL(HAVE_ACCEL_GALCORE, test x$ACCEL_GALCORE = xyes)
AC_MSG_RESULT([$ACCEL_GALCORE])


# Etnaviv GPU acceleration
AC_ARG_ENABLE(etnaviv,
	      AC_HELP_STRING([--disable-etnaviv],
			     [Disable Etnaviv acceleration support [[default=enable]]]),
	      [ACCEL_ETNAVIV="$enableval"],
	      [ACCEL_ETNAVIV=auto])

AC_ARG_WITH(etnaviv-source,
            AC_HELP_STRING([--with-etnaviv-source=PATH],
                           [specify directory for etnaviv source tree [[default=unset]]]),
            [etnaviv_source="$withval"])

AC_ARG_WITH(etnaviv-include,
            AC_HELP_STRING([--with-etnaviv-include=PATH],
                           [specify directory for installed etnaviv include files [[default=unset]]]),
            [etnaviv_include="$withval"])

AC_ARG_WITH(etnaviv-lib,
            AC_HELP_STRING([--with-etnaviv-lib=PATH],
                           [specify directory for installed etnaviv library [[default=unset]]]),
            [etnaviv_lib="$withval"])

AS_IF([test x$ACCEL_ETNAVIV != xno],
      [
   AS_IF([test x$etnaviv_source != x],
         [
           ETNAVIV_CFLAGS="-I$etnaviv_source/src"
           ETNAVIV_LIBS="-L$etnaviv_source/src/etnaviv"
         ],
         [
           AS_IF([test x$etnaviv_include = x],
           [
             PKG_CHECK_MODULES(ETNAVIV, [libetnaviv >= 1.0.0], [PKGCFG_ETNAVIV=yes], [PKGCFG_ETNAVIV=no])
           ],
           [
             ETNAVIV_CFLAGS="-I$etnaviv_include"
             ETNAVIV_LIBS="-L$etnaviv_lib"
           ])
         ])
   saved_LDFLAGS="$LDFLAGS"
   saved_CFLAGS="$CFLAGS"
   LDFLAGS="$LDFLAGS $ETNAVIV_LIBS"
   CFLAGS="$CFLAGS $ETNAVIV_CFLAGS"
   AC_CHECK_LIB([etnaviv], [viv_open],
                [ACCEL_ETNAVIV=yes],
                [ACCEL_ETNAVIV=no])
   AC_CHECK_LIB([etnaviv], [etna_bo_from_dmabuf],
                [DUMMY=y],
                [AC_MSG_ERROR([etnaviv support requires etna_bo_from_dmabuf()])])
   AC_CHECK_LIB([etnaviv], [etna_bo_from_usermem_prot],
                [DUMMY=y],
                [AC_MSG_ERROR([etnaviv support requires etna_bo_from_usermem_prot()])])
   LDFLAGS="$saved_LDFLAGS"
   CFLAGS="$saved_CFLAGS"
   AS_IF([test x$PKGCFG_ETNAVIV = xno], [ETNAVIV_LIBS+=" -letnaviv"])
   AS_IF([test x$ACCEL_ETNAVIV = xyes], [DRIVERS="etnaviv_gpu $DRIVERS"])
])
AC_MSG_CHECKING([whether to build Etnaviv acceleration support])
AS_IF([test x$ACCEL_ETNAVIV = xyes],
      [AC_DEFINE(HAVE_ACCEL_ETNAVIV,1,[Enable Etnaviv acceleration support])])
AM_CONDITIONAL(HAVE_ACCEL_ETNAVIV, test x$ACCEL_ETNAVIV = xyes)
AC_MSG_RESULT([$ACCEL_ETNAVIV])


AC_ARG_ENABLE(dri2, AC_HELP_STRING([--disable-dri2],
		[Disable DRI support [[default=auto]]]),
		[DRI2="$enableval"],
		[DRI2=auto])

# Checks for pkg-config packages
PKG_CHECK_MODULES(XORG, [xorg-server >= 1.9.99.1 xproto fontsproto])
sdkdir=$(pkg-config --variable=sdkdir xorg-server)

# Checks for required extensions
XORG_DRIVER_CHECK_EXT(RANDR, randrproto)
XORG_DRIVER_CHECK_EXT(RENDER, renderproto)
XORG_DRIVER_CHECK_EXT(XV, videoproto)

# Obtain compilter/linker options for the driver dependencies
PKG_CHECK_MODULES(DRM, [libdrm >= 2.4.38])
PKG_CHECK_MODULES(DRMARMADA, [libdrm_armada >= 2.0.0])

save_LDFLAGS="$LDFLAGS"
LDFLAGS="$LDFLAGS $DRMARMADA_LDFLAGS"
AC_CHECK_LIB([drm_armada], [drm_armada_bo_create_size],
	     [AC_DEFINE(HAVE_DRM_ARMADA_BO_CREATE_SIZE,1,[Use drm_armada_bo_create_size API])])
LDFLAGS="$save_LDFLAGS"

PKG_CHECK_MODULES(DRI2, [dri2proto >= 2.6], , DRI2=no)

# Check those options requiring DRM support
if test x$DRI2 != xno; then
   save_CFLAGS=$CFLAGS
   CFLAGS="$XORG_CFLAGS $DRM_CFLAGS $DRI_CFLAGS $DRI2_CFLAGS"
   AC_CHECK_HEADERS([dri2.h], DRI2=yes, DRI2=no, [#include <dixstruct.h>])
   CFLAGS=$save_CFLAGS
fi
AC_MSG_CHECKING([whether to include DRI2 support])
AM_CONDITIONAL(HAVE_DRI2, test x$DRI2 = xyes)
AC_MSG_RESULT([$DRI2])
if test x$DRI2 = xyes; then
   AC_DEFINE(HAVE_DRI2,1,[Enable DRI2 driver support])
fi

# Checks for header files.
AC_HEADER_STDC

AC_SUBST([XORG_CFLAGS])
AC_SUBST([LIBGAL_CFLAGS])
AC_SUBST([LIBGAL_LIBS])
AC_SUBST([DRIVER_NAME])
AC_SUBST([ETNAVIV_CFLAGS])
AC_SUBST([ETNAVIV_LIBS])
AC_SUBST([moduledir])
AC_SUBST([DRIVERS])

AC_OUTPUT([
	Makefile
	common/Makefile
	etnaviv/Makefile
	man/Makefile
	src/Makefile
	vivante/Makefile
])
