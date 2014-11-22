#! /bin/sh
#
# Run the various GNU autotools to bootstrap the build
# system.  Should only need to be done once.

srcdir=`dirname $0`
test -z "$srcdir" && srcdir=.

ORIGDIR=`pwd`
cd $srcdir

mkdir -p $srcdir/m4
autoreconf --install || exit 1

cd $ORIGDIR || exit $?

$srcdir/configure --enable-maintainer-mode "$@"
