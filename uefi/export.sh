#!/bin/bash
# Save our changes to the Mu-Silicium tree (../../../Mu-Silicium) back into this directory:
# mu-silicium.patch and mu-basecore.patch for the files upstream has, files/ for the new ones.
set -e
cd "$(dirname "$(readlink -f "$0")")"
MU=../../../Mu-Silicium
git -C $MU rev-parse HEAD > BASE
git -C $MU diff > mu-silicium.patch
git -C $MU/Mu_Basecore diff > mu-basecore.patch
rm -rf files
mkdir -p files/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib
cp $MU/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/XnuLoader.[ch] files/Silicon/Silicium/SiliciumPkg/Library/BootManagerLib/
find files -name __pycache__ -prune -exec rm -rf {} +
echo "uefi/export.sh: base $(cut -c1-12 BASE), $(wc -l < mu-silicium.patch) + $(wc -l < mu-basecore.patch) patch lines"
