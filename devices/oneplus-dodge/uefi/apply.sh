#!/bin/bash
# Make ../../../Mu-Silicium: Project-Silicium/Mu-Silicium at the commit our port was
# made on (upstream already carries the dodgePkg platform), with this port put into
# it: a patch for the files upstream has, one inside the Mu_Basecore submodule, and
# the XnuLoader sources. See README.md here for how to build it.
#
# The clone is as shallow as it can be (one commit, submodules included); when the
# tree is already at BASE with the port applied, the script does nothing (a CI cache
# can restore that state and skip the network entirely).
set -e
cd "$(dirname "$(readlink -f "$0")")"
MU=../../../Mu-Silicium
BASE=$(cat BASE)

if [ -d "$MU/.git" ]; then
	if [ "$(git -C "$MU" rev-parse HEAD)" = "$BASE" ] && [ -f "$MU/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/XnuLoader.c" ]; then
		echo "uefi/apply.sh: $MU already at the base with the port applied"
		exit 0
	fi
	echo "uefi/apply.sh: $MU exists at another state; remove it to start over" >&2
	exit 1
fi

echo "uefi/apply.sh: cloning Mu-Silicium ($BASE), shallow"
git init -q "$MU"
git -C "$MU" remote add origin https://github.com/Project-Silicium/Mu-Silicium.git
git -C "$MU" fetch -q --depth 1 origin "$BASE"
git -C "$MU" checkout -q FETCH_HEAD
git -C "$MU" submodule update --init --recursive --depth 1
git -C "$MU" apply --whitespace=nowarn "$PWD/mu-silicium.patch"
git -C "$MU/Mu_Basecore" apply --whitespace=nowarn "$PWD/mu-basecore.patch"
cp -r files/. "$MU/"
echo "uefi/apply.sh: done; next: uefi/README.md"
