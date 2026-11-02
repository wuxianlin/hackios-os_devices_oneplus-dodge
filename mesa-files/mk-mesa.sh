#!/bin/bash
# Build Mesa for the iOS userland: the OpenGL ES library behind userland/src/eagl.c (see there and
# userland/src/gl_osmesa.c). Mesa is fetched (a pinned release), patched with tools/mesa/*.patch and
# built with the cross compiler of the kernel (xnu-deps/host/bin/xnu-clang, see xnu-deps/setup.sh)
# against the iOS SDK that userland/build.sh uses; nothing of it is in the repository.
#
#   DRIVERS="softpipe,freedreno"
#                        the gallium drivers to build. softpipe: a rasterizer in plain C; it needs no
#                        LLVM and nothing of the machine but its processor. freedreno: the Adreno's;
#                        it talks to the kernel's GPU driver (the service RedwoodGPU) through
#                        tools/mesa/redwood-drm, which stands in for libdrm (tools/mesa/freedreno-darwin.patch
#                        makes Mesa build it where there is no DRM). The library takes the GPU when the
#                        kernel has one and the environment does not say OSMESA_SOFTWARE=1.
#
# What comes out, in xnu-deps/build/mesa/out:
#   libOSMesa.8.dylib    everything in one library (install name /redwood/lib/libOSMesa.8.dylib): the
#                        OpenGL (ES) entry points gl*, the compiler of the shading language, the drivers,
#                        and the off-screen interface that hands out contexts
#   exported-gl.txt      the gl* functions it exports, and
#   gl.xml               their prototypes (the Khronos registry as Mesa has it): for userland/mkeagl.py
# and with AIR=1, the two things the Metal subsystem needs (docs/air.md, docs/metal.md 10.12):
#   air_run              the test program: a Metal shader translated, drawn or dispatched, read back
#   libredwoodair.dylib  the same path as a library (tools/mesa/air/air_metal.h), install name
#                        /redwood/lib/libredwoodair.dylib, which is what userland's Metal driver
#                        links against to compile a function Metal hands it
# and with TURNIP=1 (26.x only, REBASE=26), Mesa's Vulkan driver for the Adreno instead of all of the above:
#   libvulkan_freedreno.dylib
#                        Turnip on its msm backend, through tools/mesa/redwood-drm like freedreno
#                        (tools/mesa/rebase-26/0004-turnip-msm-darwin.patch), install name
#                        /redwood/lib/libvulkan_freedreno.dylib; loaded as an ICD (vk_icdGetInstanceProcAddr)
#   tu_probe             tools/mesa/turnip-probe: enumerates the GPU, creates a device, and checks a buffer
#                        fill and copy, a compute shader and a drawn triangle by reading them back
# Needs on this machine: bison, flex, python3 with venv (Mako and a pinned Meson are installed into a
# private environment), llvm-ar, and the iOS cross toolchain -- $THEOS/toolchain/linux/iphone (cctools'
# ld64 and install_name_tool, x86_64 binaries that run under qemu-user or Rosetta on an arm64 host) plus
# $THEOS/sdks/iPhoneOS16.5.sdk. All of it is baked into the hackios-dev image (docker/Dockerfile pins
# the theos commit and hashes the tools), so `docker/dev.sh` is the supported way to run this.
#
# This build is NOT reproducible byte for byte, so do not compare one of its outputs with another
# by hash. Measured on 20 October: two builds from the same source tree, the second with
# build-softpipe-freedreno deleted first, gave different libOSMesa.8.dylib and air_run -- same size
# to the byte, 9 132 774 of 14 855 048 bytes different in the library and 4 171 752 of 9 789 824 in
# air_run, with 5 of the 6 named text symbols at new addresses. So the layout moves between builds.
# What moves it was not established; it is not source filenames, which -Db_ndebug=true compiles out.
#
# This is Mesa's property and not the build system's, which is worth knowing before anyone
# distrusts every hash here: the userland programs reproduce exactly. prebuilt/userland/init built
# in one working copy came out byte for byte identical to the same source built in another, at a
# different time -- so there a hash does compare builds, and only for Mesa it does not.
#
# The practical consequence: a hash answers whether this is the same FILE, never whether it holds
# the same CODE, and the size answers neither -- the one-line ir3 rounding patch
# (tools/mesa/ir3-round-default.patch) produced a library of exactly the same 14 855 048 bytes as
# the build without it. To tell whether a patch is in a build, read the source in
# xnu-deps/build/mesa/mesa-$VERSION, which is where this script unpacks and patches it.
set -e
VERSION=${VERSION:-25.0.7}
SHA256=${SHA256:-592272df3cf01e85e7db300c449df5061092574d099da275d19e97ef0510f8a6}
# The 26.x experiment baseline (docs/analysis/mesa-rebase-2026-10.md section 6): the fork's patches survive
# 26.2.5 with an adaptation layer, so the trial runs as
#   VERSION=26.2.5 SHA256=794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5 REBASE=26 tools/mk-mesa.sh
# REBASE=26 lays tools/mesa/rebase-26/*.patch over the rest -- a subdirectory, so the glob below misses it
# on purpose: the layer only belongs on 26.x, never on 25.0.7.
REBASE=${REBASE:-}
ROOT=$(cd "$(dirname "$(readlink -f "$0")")/.." && pwd)
source "$ROOT/xnu-env.sh"
# the stamp expressions in the one text build.sh sources too: what is written here is read there,
# so the pair cannot drift into "a rebuild every build" or "a stale library shipped quietly"
source "$ROOT/tools/mesa-stamp.sh"
B=$ROOT/xnu-deps/build/mesa
SDK=$THEOS/sdks/iPhoneOS16.5.sdk
CC=$ROOT/xnu-deps/host/bin/xnu-clang
# xnu-clang links through $ROOT/xnu-deps/host/bin/ld64 -- that exact path is where the kernel build
# looks for the linker, and nothing but this script puts it there: an untracked symlink onto the
# theos toolchain, which is why the container image, not the repository, is its source of truth.
LD64=$ROOT/xnu-deps/host/bin/ld64
[ -e "$LD64" ] || ln -sfn "$THEOS/toolchain/linux/iphone/bin/ld" "$LD64"
# the C++ name of the same wrapper, which xnu-deps/setup.sh also makes (line: link xnu-clang
# host/bin/xnu-clang++); Mesa needs the C++ compiler, and mesa does not run setup.sh
[ -e "$ROOT/xnu-deps/host/bin/xnu-clang++" ] || ln -s xnu-clang "$ROOT/xnu-deps/host/bin/xnu-clang++"
# the same toolchain holds install_name_tool; the bare name is only right on macOS, and it was
# never on the PATH of the old containers, so resolve it rather than assume it
INTOOL=$THEOS/toolchain/linux/iphone/bin/install_name_tool
[ -x "$INTOOL" ] || INTOOL=install_name_tool
# the install name a dylib actually carries, read back out of it: install_name_tool can exit 0 on
# a file it did not fix the way the device needs, and the id is only checked by dyld on the
# device -- which is the most expensive place. llvm-objdump is the container's, otool macOS'.
dylib_id() {
	if command -v llvm-objdump >/dev/null 2>&1; then
		llvm-objdump --macho --private-headers "$1" | awk '/LC_ID_DYLIB/{id=1} id && $1=="name"{print $2; exit}'
	else
		otool -D "$1" | sed -n 2p
	fi
}
mkdir -p "$B/out"
cd "$B"

# The sources are the archive's with tools/mesa/*.patch on top. When the patches change, a tree that was
# patched with the earlier ones is of no use (and builds without a word into a library that does not match
# the rest): the sources are taken from the archive again and everything is built anew, about a minute.
REBASE_PATCHES=""
if [ "$REBASE" = 26 ]; then REBASE_PATCHES="$ROOT"/tools/mesa/rebase-26/*.patch; fi
# LC_ALL=C inside the substitution, before the globs expand: a shell sorts a glob in the locale's
# collation, the same 23 October trap the stamp hashed around -- only here it decides
# .redwood-patches, and a host/container disagreement costs an unpack and re-patch of the tree.
PATCHES=$(LC_ALL=C; export LC_ALL; cat "$ROOT"/tools/mesa/*.patch $REBASE_PATCHES | sha256sum | cut -d' ' -f1)
if [ ! -d mesa-$VERSION ] || [ "$(cat mesa-$VERSION/.redwood-patches 2>/dev/null)" != "$PATCHES" ]; then
	[ -f mesa-$VERSION.tar.xz ] || curl -sL --fail -o mesa-$VERSION.tar.xz https://archive.mesa3d.org/mesa-$VERSION.tar.xz
	echo "$SHA256  mesa-$VERSION.tar.xz" | sha256sum -c --quiet
	rm -rf mesa-$VERSION build-*
	tar xJf mesa-$VERSION.tar.xz
	# No reject may survive from an earlier application: with one in the tree, "did this run add a
	# reject" has no answer, and an old one reads as a capability that went missing (28 October).
	find mesa-$VERSION \( -name '*.rej' -o -name '*.orig' \) -delete
	# On 26.x some hunks of the old patches fail or skip, which is expected: 0003-restore-* re-establishes
	# what they carried (tools/mesa/rebase-26/README.md). Only there, so a 25.0.7 build still stops on a
	# patch that no longer fits.
	# --batch, and the output kept: without it patch asks "File to patch:" and reads the closed stdin as
	# "skip the hunk", so a patch that no longer fits leaves neither a nonzero status nor a line anyone
	# reads; the prompt and "malformed patch" in this log are its only trace.
	PATCHLOG=patch.log
	: > "$PATCHLOG"
	for p in "$ROOT"/tools/mesa/*.patch; do patch -s --batch --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p" >> "$PATCHLOG" 2>&1 || [ "$REBASE" = 26 ]; done
	if [ "$REBASE" = 26 ]; then for p in "$ROOT"/tools/mesa/rebase-26/*.patch; do patch -s --batch --no-backup-if-mismatch -d mesa-$VERSION -p1 < "$p" >> "$PATCHLOG" 2>&1; done; fi
	# What the loop above dropped, said out loud once: 26.x is allowed to drop the old hunks, every other
	# version is not, and a build that dropped one on those is the case this check exists for.
	REJECTS=$(find mesa-$VERSION -name '*.rej' | LC_ALL=C sort)
	DROPPED=$(grep -cE 'File to patch:|malformed patch' "$PATCHLOG" || true)
	if [ -n "$REJECTS" ] || [ "$DROPPED" != 0 ]; then
		echo "mk-mesa.sh: $(printf '%s\n' "$REJECTS" | grep -c . || true) reject file(s), $DROPPED skip(s):" >&2
		printf '%s\n' "$REJECTS" | grep . >&2 || true
		grep -nE 'File to patch:|malformed patch' "$PATCHLOG" >&2 || true
		[ "$REBASE" = 26 ] || exit 1
	fi
	echo "$PATCHES" > mesa-$VERSION/.redwood-patches
	# The log goes next to the artifacts: what a round's manifest records has to be readable from
	# the same place as the library it describes, or "which patch dropped what" is answered from a
	# file only the build machine's build directory holds.
	cp "$PATCHLOG" out/patch.log
	# The rejects as a list of paths: a reject decides which hunk did not reach the build, and that
	# fact appears in no digest, so the manifest carries it as its own field. Written as an empty file
	# rather than skipped, so "no rejects this time" and "this build recorded none" read the same.
	if [ -n "$REJECTS" ]; then printf '%s\n' "$REJECTS" > out/patch-rejects.txt; else : > out/patch-rejects.txt; fi
fi
# ours, kept in the repository: always the current ones
rm -rf mesa-$VERSION/src/redwood-drm
cp -r "$ROOT/tools/mesa/redwood-drm" mesa-$VERSION/src/redwood-drm
# the AIR frontend (docs/air.md): the Metal runtime will call it in this
# library's own process, and tools/mesa/air-frontend.patch is the meson that
# reaches it. Copied rather than linked, because this tree is cross-built.
rm -rf mesa-$VERSION/src/compiler/air
cp -r "$ROOT/tools/mesa/air" mesa-$VERSION/src/compiler/air
if [ ! -x venv/bin/python3 ]; then
	python3 -m venv venv
	# meson is pinned, not apt's: 26.x meson.build requires >= 1.4 and Ubuntu noble ships 1.3,
	# and a pinned builder is what makes the cross build reproducible across machines
	# (docs/analysis/mesa-rebase-2026-10.md section 9).
	venv/bin/pip -q install mako pyyaml packaging "meson==1.12.1"
fi
export PATH=$B/venv/bin:$PATH

# the compiler is the kernel's wrapper: it names the target (arm64-apple-ios) and the linker (ld64)
cat > cross.ini <<INI
[binaries]
c = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK']
cpp = ['$CC++', '-arch', 'arm64', '-isysroot', '$SDK', '-fuse-cxa-atexit']
objc = ['$CC', '-arch', 'arm64', '-isysroot', '$SDK']
ar = 'llvm-ar'
ranlib = 'llvm-ranlib'
strip = 'llvm-strip'
pkg-config = 'false'

[host_machine]
system = 'darwin'
subsystem = 'ios'
cpu_family = 'aarch64'
cpu = 'arm64'
endian = 'little'

[properties]
needs_exe_wrapper = true
INI

if [ "${TURNIP:-}" = 1 ]; then
	[ "$REBASE" = 26 ] || { echo "mk-mesa.sh: TURNIP=1 needs REBASE=26 (the patch is tools/mesa/rebase-26/0004-turnip-msm-darwin.patch)"; exit 1; }
	command -v glslangValidator >/dev/null || { echo "mk-mesa.sh: TURNIP=1 needs glslangValidator (apt-get install glslang-tools)"; exit 1; }
	# Darwin has no pthread_condattr_setclock; Turnip's only caller asks for CLOCK_MONOTONIC on a
	# condition variable it waits on with relative timeouts. b_lundef=false: the library leaves Mesa's
	# entry points this build omits to dyld (0004's meson.build); a global -undefined dynamic_lookup
	# would instead make meson's function checks pass for functions iOS lacks, secure_getenv first.
	[ -f build-turnip/build.ninja ] || meson setup build-turnip mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
		--prefix=/redwood --libdir=lib --buildtype=release -Db_ndebug=true \
		-Dplatforms= -Dgallium-drivers= -Dfreedreno-kmds=msm -Dvulkan-drivers=freedreno -Dglx=disabled -Degl=disabled -Dgbm=disabled \
		-Dosmesa=false -Dllvm=disabled -Dgles1=disabled -Dgles2=disabled -Dopengl=false \
		-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
		-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-va=disabled \
		-Dlmsensors=disabled -Dbuild-tests=false '-Dcpp_args=-Dpthread_condattr_setclock(a,b)=0' -Db_lundef=false \
		> meson-setup-turnip.log
	ninja -C build-turnip src/freedreno/vulkan/libvulkan_freedreno.dylib > ninja-turnip.log
	llvm-strip -x -o out/libvulkan_freedreno.dylib build-turnip/src/freedreno/vulkan/libvulkan_freedreno.dylib
	"$INTOOL" -id /redwood/lib/libvulkan_freedreno.dylib out/libvulkan_freedreno.dylib
	[ "$(dylib_id out/libvulkan_freedreno.dylib)" = /redwood/lib/libvulkan_freedreno.dylib ] || {
		echo "mk-mesa.sh: libvulkan_freedreno.dylib does not carry the install name /redwood/lib/libvulkan_freedreno.dylib"; exit 1; }
	llvm-nm -gU out/libvulkan_freedreno.dylib | grep -q ' _vk_icdGetInstanceProcAddr$' || {
		echo "mk-mesa.sh: libvulkan_freedreno.dylib does not export vk_icdGetInstanceProcAddr"; exit 1; }
	mkdir -p turnip-probe/shaders
	for shader in fill.comp tri.vert tri.frag; do
		glslangValidator -V --vn "spv_${shader/./_}" -o "turnip-probe/shaders/${shader/./_}.h" "$ROOT/tools/mesa/turnip-probe/$shader" > /dev/null
	done
	"$CC" -arch arm64 -isysroot "$SDK" -O1 -Wall -Imesa-$VERSION/include -Iturnip-probe \
		-o out/tu_probe "$ROOT/tools/mesa/turnip-probe/tu_probe.c"
	echo "mk-mesa.sh: Mesa $VERSION (Turnip): $B/out/libvulkan_freedreno.dylib, $(stat -c %s out/libvulkan_freedreno.dylib) bytes; $B/out/tu_probe"
	exit 0
fi
DRIVERS=${DRIVERS:-softpipe,freedreno}
BUILD=build-${DRIVERS//,/-}
# 26.x dropped the gallium state trackers vdpau and xa, so those options only exist up to 25.x;
# meson hard-errors on an unknown -D, which means the flag list has to follow the version.
LEGACY_TRACKERS="-Dgallium-vdpau=disabled -Dgallium-xa=disabled"
case "$VERSION" in
	1.*|2[0-5].*) ;;
	*) LEGACY_TRACKERS="" ;;
esac
[ -f $BUILD/build.ninja ] || meson setup $BUILD mesa-$VERSION --cross-file cross.ini --wrap-mode=nofallback \
	--prefix=/redwood --libdir=lib --buildtype=release -Db_ndebug=true \
	-Dplatforms= -Dgallium-drivers="$DRIVERS" -Dfreedreno-kmds=msm -Dvulkan-drivers= -Dglx=disabled -Degl=disabled -Dgbm=disabled \
	-Dosmesa=true -Dllvm=disabled -Dshared-glapi=enabled -Dgles1=disabled -Dgles2=enabled -Dopengl=true \
	-Dzstd=disabled -Dxmlconfig=disabled -Dexpat=disabled -Dshader-cache=disabled -Dvalgrind=disabled \
	-Dlibunwind=disabled -Dglvnd=disabled -Dvideo-codecs= -Dtools= -Dgallium-va=disabled \
	$LEGACY_TRACKERS -Dlmsensors=disabled -Dbuild-tests=false > meson-setup.log
ninja -C $BUILD > ninja.log
# air_run draws with a translated Metal shader and reads the pixels back; it is
# the only way to find out whether the Adreno takes work that did not come from
# the window server. Built only when asked, since it is a test program.
if [ "${AIR:-}" = 1 ]; then
	ninja -C $BUILD src/gallium/drivers/freedreno/air_run >> ninja.log
	cp $BUILD/src/gallium/drivers/freedreno/air_run out/air_run
	echo "mk-mesa.sh: $B/out/air_run ($(stat -c %s out/air_run) bytes)"
	# and the same path as a library, which is not a test program: the Metal driver links
	# against it to compile the bitcode Metal hands a function. Stripped and named the way
	# libOSMesa is below -- -x keeps the global symbols, which here are the rwa_* of
	# tools/mesa/air/air_metal.h, and the install name has to be the path on the device
	# because meson's own is @rpath.
	ninja -C $BUILD src/gallium/drivers/freedreno/libredwoodair.dylib >> ninja.log
	llvm-strip -x -o out/libredwoodair.dylib $BUILD/src/gallium/drivers/freedreno/libredwoodair.dylib
	"$INTOOL" -id /redwood/lib/libredwoodair.dylib out/libredwoodair.dylib
	# the interface the driver calls, kept as a manifest beside the library: the count printed
	# here used to be allowed to be 0 (a strip that took rwa_* with it, or a meson that stopped
	# building the sources, left a green build and a driver that dlopens a library it cannot
	# call), and the 31 of docs/analysis/mesa-rebase-2026-10.md 9 is a ledger, not an assertion --
	# so the list itself lands in out/exports-rwa.txt, its hash beside the size in the echo.
	llvm-nm -gU out/libredwoodair.dylib | awk '{ print $NF }' | LC_ALL=C sort > out/exports-rwa.txt
	RWA=$(grep -c '^_rwa_' out/exports-rwa.txt)
	[ "$RWA" -gt 0 ] || { echo "mk-mesa.sh: libredwoodair.dylib exports no rwa_* at all: the interface of tools/mesa/air/air_metal.h is not in the library"; exit 1; }
	[ "$(dylib_id out/libredwoodair.dylib)" = /redwood/lib/libredwoodair.dylib ] || {
		echo "mk-mesa.sh: libredwoodair.dylib does not carry the install name /redwood/lib/libredwoodair.dylib"; exit 1; }
	echo "mk-mesa.sh: $B/out/libredwoodair.dylib ($(stat -c %s out/libredwoodair.dylib) bytes," \
		"$RWA rwa_* functions, interface $(sha256sum < out/exports-rwa.txt | cut -d' ' -f1))"
	# what this library was built from, so userland/build.sh can tell that it is older than the
	# sources and come back here instead of shipping it beside a driver that disagrees with it;
	# the expression and the byte-order sort are in tools/mesa-stamp.sh, the one text build.sh
	# computes the comparison value from. Written after the two assertions: a failed check leaves
	# the old stamp, and a build.sh that comes back here does not find a green pair.
	mesa_air_stamp "$ROOT" > out/built-from-air
fi

llvm-strip -x -o out/libOSMesa.8.dylib $BUILD/src/gallium/targets/osmesa/libOSMesa.8.dylib
"$INTOOL" -id /redwood/lib/libOSMesa.8.dylib out/libOSMesa.8.dylib
[ "$(dylib_id out/libOSMesa.8.dylib)" = /redwood/lib/libOSMesa.8.dylib ] || {
	echo "mk-mesa.sh: libOSMesa.8.dylib does not carry the install name /redwood/lib/libOSMesa.8.dylib"; exit 1; }
llvm-nm -gU out/libOSMesa.8.dylib | awk '$3 ~ /^_gl/ { print substr($3, 2) }' > out/exported-gl.txt
# mkeagl.py consumes this list next and the whole table of eagl_gen.h is only as good as it is:
# an empty one is a library exporting no gl* at all, and one without glDrawArrays is a build of
# the wrong target -- either way, stop here rather than in front of CoreAnimation on the device.
[ -s out/exported-gl.txt ] || { echo "mk-mesa.sh: exported-gl.txt is empty: the library exports no gl* at all"; exit 1; }
grep -qx glDrawArrays out/exported-gl.txt || { echo "mk-mesa.sh: exported-gl.txt has no glDrawArrays: this is not an OpenGL ES 2 library"; exit 1; }
# 26.x moved the Khronos registry inside the tree (src/mapi/glapi/registry, then src/mesa/glapi/glapi/registry)
glxml=mesa-$VERSION/src/mapi/glapi/registry/gl.xml
[ -f "$glxml" ] || glxml=mesa-$VERSION/src/mesa/glapi/glapi/registry/gl.xml
cp "$glxml" out/gl.xml
# what fell off the tolerant 26.x patching: `|| [ "$REBASE" = 26 ]` above lets a hunk fail or skip,
# and the only trace is a .rej in the unpacked tree, which the next rm -rf buries. Collected here
# so the drift is a file read beside the library instead of an argument nobody kept -- recorded,
# never fatal, because 26.x is expected to carry some (tools/mesa/rebase-26/README.md). Only the
# suffixes GNU patch itself writes: an earlier form also collected *.frag and caught two of
# Mesa's own shader files (src/tool/gamma, src/vulkan/overlay-layer) for it.
{ echo "# mesa-$VERSION, patches $PATCHES"
	find mesa-$VERSION \( -name '*.rej' -o -name '*.orig' \) -type f -print | LC_ALL=C sort
} > out/patch-survey.txt
# what this library was built from, so userland/build.sh can tell that it is older than the
# sources and come back here instead of shipping it beside a driver that disagrees with it. The
# expression, the byte-order sort and why they are that way are in tools/mesa-stamp.sh, which
# build.sh sources to compute the value this file is compared against. Written last, as always:
# the stamp is what says a successful build happened.
mesa_from_stamp "$ROOT" "$VERSION" "$REBASE" > out/built-from
echo "mk-mesa.sh: Mesa $VERSION ($DRIVERS): $B/out/libOSMesa.8.dylib, $(stat -c %s out/libOSMesa.8.dylib) bytes," \
	"$(wc -l < out/exported-gl.txt) gl* functions"
