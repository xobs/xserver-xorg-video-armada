#
# Marvell Armada DRM-based driver
#
# Written by Russell King, 2012, derived in part from the
# Intel xorg X server driver.
#

# Turn off -Wnested-externs - these are a good thing because it allows
# information hiding and helps prevent misuse of private externs.
# Turn off -Wcast-qual - this makes stuff like string assignment
# too noisy.
# Turn off -Wredundant-decls - Xorg headers seem to contain a lot
# of this, so why it's in xorg-macros.m4... maybe more of a wish?
# Turn off -Wshadow - Xorg headers seem to declare a lot of globals
# which can conflict - index, range, etc.
AM_CFLAGS = $(filter-out -Wnested-externs -Wcast-qual -Wredundant-decls \
	-Werror=write-strings -Wshadow,$(CWARNFLAGS)) \
	$(XORG_CFLAGS) $(DRMARMADA_CFLAGS) $(UDEV_CFLAGS) \
	-I$(top_srcdir)/common

armada_drv_la_LTLIBRARIES = armada_drv.la
armada_drv_la_LDFLAGS = -module -avoid-version
armada_drv_la_LIBADD = $(DRMARMADA_LIBS) $(UDEV_LIBS) \
			$(top_builddir)/common/libcommon.la
armada_drv_ladir = @moduledir@/drivers
armada_drv_la_SOURCES = armada_accel.h \
			armada_module.c \
			armada_drm.c \
			armada_drm.h \
			armada_drm_xv.c \
			armada_fourcc.h \
			armada_ioctl.h \
			common_drm.c \
			common_drm.h \
			common_drm_helper.h
if HAVE_DRI2
armada_drv_la_SOURCES += \
			common_drm_dri2.c \
			common_drm_dri2.h
endif

if HAVE_PRESENT
armada_drv_la_SOURCES += common_present.c
endif
