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
	$(XORG_CFLAGS) $(LIBGAL_CFLAGS) $(DRMARMADA_CFLAGS) $(UDEV_CFLAGS) \
	-I$(top_srcdir)/src -I$(top_srcdir)/common

vivante_gpu_la_LTLIBRARIES = vivante_gpu.la
vivante_gpu_la_LDFLAGS = -module -avoid-version
vivante_gpu_la_LIBADD = $(DRMARMADA_LIBS) $(LIBGAL_LIBS) \
			$(top_builddir)/common/libcommon.la
vivante_gpu_ladir = @moduledir@/drivers
vivante_gpu_la_SOURCES = \
			vivante.c \
			vivante_accel.c \
			vivante_utils.c
if HAVE_DRI2
vivante_gpu_la_SOURCES += vivante_dri2.c
vivante_gpu_la_LIBADD += $(DRI_LIBS)
endif
