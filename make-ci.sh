#!/bin/bash

machine=$(uname -sm | tr ' ' '-')
echo $machine

function download() {
	url="$1"
	target="$2"
	if [ -e "$target" ]; then return; fi
	curl "$url" -LJo "$target"
}

topdir="$(pwd)"
mkdir -p dependencies

download https://github.com/auriocus/kbskit/releases/download/latest/kbskit_$machine.tar.bz2 dependencies/kbskit.tar.bz2

( cd dependencies
  tar xvf kbskit.tar.bz2 )


builddir="$topdir/build"
distdir="$topdir/dist"
kbskitdir="$topdir/dependencies/kbskit_$machine"
tcldir="$kbskitdir/lib"

rm -rf "$builddir"

autoconf

./configure LDFLAGS="-L$tcldir" --with-tcl="$tcldir" --with-tk="$tcldir" --prefix="$builddir" --libdir="$builddir/lib" --exec-prefix="$kbskitdir"
make
make install

mkdir -p "$distdir"
tar cvjf "$distdir/tksvg_$machine.tar.bz2" -C "$builddir/lib" $(basename -a "$builddir/lib/"*)
