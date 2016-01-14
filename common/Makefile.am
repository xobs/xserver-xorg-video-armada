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
	$(XORG_CFLAGS)

noinst_LTLIBRARIES = libcommon.la
libcommon_la_SOURCES = \
		backlight.c \
		bo-cache.c \
		bo-cache.h \
		boxutil.c \
		boxutil.h \
		compat-api.h \
		compat-list.h \
		cpu_access.h \
		drawable_desc.c \
		fd.c \
		fbutil.h \
		gal_extension.h \
		get_first_pixel.c \
		glyph_assemble.c \
		glyph_assemble.h \
		glyph_cache.c \
		glyph_cache.h \
		glyph_extents.c \
		glyph_extents.h \
		mark.c \
		mark.h \
		pamdump.c \
		pamdump.h \
		pictureutil.h \
		picture_desc.c \
		picture_solid.c \
		pixmaputil.c \
		pixmaputil.h \
		prefetch.h \
		transform.c \
		unaccel.c \
		unaccel.h \
		unaccel_render.c \
		utils.h \
		xv_attribute.c \
		xv_attribute.h \
		xv_image_format.c \
		xv_image_format.h \
		xvbo.h
