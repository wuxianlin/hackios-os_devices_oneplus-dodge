#!/bin/bash
# Make ../../../Mu-Silicium: a clone of Project-Silicium/Mu-Silicium at the commit our port was
# made on (upstream already carries the dodgePkg platform), with this port put into it: a patch
# for the files upstream has, one for inside the Mu_Basecore submodule, and the XnuLoader sources.
# See README.md here for how to build it.
set -e
cd "$(dirname "$(readlink -f "$0")")"
MU=../../../Mu-Silicium
if [ -d "$MU/.git" ] && [ -f "$MU/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/XnuLoader.c" ]; then
	echo "uefi/apply.sh: $MU already at the base with the port applied"
	exit 0
fi
[ -e $MU ] && { echo "uefi/apply.sh: $MU exists but is not a clone with the port; remove it"; exit 1; }
git clone https://github.com/Project-Silicium/Mu-Silicium.git $MU
git -C $MU checkout -q "$(cat BASE)"
git -C $MU submodule update --init --recursive
git -C $MU apply --whitespace=nowarn "$PWD/mu-silicium.patch"
git -C $MU/Mu_Basecore apply --whitespace=nowarn "$PWD/mu-basecore.patch"
cp -r files/. $MU/
echo "uefi/apply.sh: done; next: uefi/README.md"
