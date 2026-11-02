# The two stamps that pair a Mesa build with the userland that ships it. They live here as ONE
# text because userland/build.sh reads them and tools/mk-mesa.sh writes them, and the pair has to
# agree word for word: until 25 October the expressions were hand-copied on both sides (the note
# "both sides here and there, or not at all" was the contract admitting it), and a drift between
# the copies is either a rebuild every build or a stale library shipped quietly.
#
#   mesa_from_stamp <root> <version> <rebase>
#                        the patches and the redwood-drm the library is built from, plus the
#                        baseline they were meant for -- the caller names VERSION/REBASE, which is
#                        where the baseline is pinned (userland/build.sh pins the branch's,
#                        mk-mesa.sh defaults to the historical one)
#   mesa_air_stamp <root>
#                        the AIR frontend tools/mesa/air/, which the hash above does not mention:
#                        libredwoodair.dylib is built from those sources and from nothing else
#
# LC_ALL=C on the sort: the file list is hashed in byte order, not in the locale's collation -- a
# 23 October check on macOS (en_US.UTF-8) read this contract as changed when it was not (bash
# sorts `air2nir.c` before `air_bitcode.c`, GNU sort puts the underscore first), and a false
# positive here means a 9-minute Mesa build nobody asked for. The functions print exactly what
# sha256sum prints ("hash  -"), so the stamps out/built-from and out/built-from-air a tree already
# holds still read as unchanged against this file.
#
# Source it: `. tools/mesa-stamp.sh`. Sourced, not executed: mk-mesa.sh needs the functions to
# write what build.sh will read, and a subprocess could not hand back the same definitions.

mesa_from_stamp() {
	{ { find "$1"/tools/mesa -maxdepth 1 -type f -name '*.patch' -print
			# the rebase layer only belongs on 26.x, never on the 25.0.7 default -- and as an `if`,
			# not as `[ ... ] && find`: under mk-mesa's `set -e` the latter ended the pipeline's
			# subshell on a 25.x run, hashing a list missing everything after the guard.
			if [ "$3" = 26 ]; then find "$1"/tools/mesa/rebase-26 -maxdepth 1 -type f -name '*.patch' -print; fi
			find "$1"/tools/mesa/redwood-drm -maxdepth 1 -type f -print; } |
			LC_ALL=C sort | xargs cat
		echo "mesa-$2 REBASE=$3"; } | sha256sum
}

mesa_air_stamp() {
	find "$1"/tools/mesa/air -maxdepth 1 -type f -print | LC_ALL=C sort | xargs cat | sha256sum
}
