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
	$(XORG_CFLAGS) $(ETNAVIV_CFLAGS) $(DRMARMADA_CFLAGS) $(UDEV_CFLAGS) \
	-I$(top_srcdir)/src -I$(top_srcdir)/common

ETNA_COMMON_SOURCES = \
	etnaviv.c \
	etnaviv_accel.c \
	etnaviv_op.c \
	etnaviv_utils.c \
	etnaviv_xv.c
ETNA_COMMON_LIBADD = \
	$(DRMARMADA_LIBS) \
	$(top_builddir)/common/libcommon.la

if HAVE_DRI2
ETNA_COMMON_SOURCES += etnaviv_dri2.c
ETNA_COMMON_LIBADD += $(DRI_LIBS)
endif

if HAVE_ACCEL_ETNAVIV
etnaviv_gpu_la_LTLIBRARIES = etnaviv_gpu.la
etnaviv_gpu_la_LDFLAGS = -module -avoid-version
etnaviv_gpu_la_LIBADD = \
	$(ETNAVIV_LIBS) \
	$(ETNA_COMMON_LIBADD)
etnaviv_gpu_ladir = @moduledir@/drivers
etnaviv_gpu_la_SOURCES = \
	$(ETNA_COMMON_SOURCES) \
	etnaviv_dummy.c \
	etnaviv_emit.c \
	etnaviv_module.c
endif

if HAVE_ACCEL_ETNADRM
etnadrm_gpu_la_LTLIBRARIES = etnadrm_gpu.la
etnadrm_gpu_la_LDFLAGS = -module -avoid-version
etnadrm_gpu_la_LIBADD = \
	$(ETNA_COMMON_LIBADD)
etnadrm_gpu_ladir = @moduledir@/drivers
etnadrm_gpu_la_SOURCES = \
	$(ETNA_COMMON_SOURCES) \
	etnadrm_emit.c \
	etnadrm_module.c \
	etnadrm.c
endif
