# 这份 `tools/mesa/` 只服务 iOS 16.5 那条线

iOS 26 的工作不要改这里。iOS 26 的权威副本是 `hackios-xnu-kernel/tools/mesa/`（同一个工作区里的另一个 submodule），`package.py` 与 `mk-mesa.sh` 取的都是那一份。

两份不合并：合并会动到 16.5 的构建路径，风险大于收益。改 iOS 26 的 `air_metal.c`、`air_metal.h` 或补丁时在 `hackios-xnu-kernel/` 里改，用 `common/ugit add -f <路径>` 提交（`add` 必须带路径）。

2026-10-28：曾有两个提交把这个目录放进 perf 的诊断 patch（`70ba33f`、`bbde1df`，`draw-perf.patch`），已删除（`dd58070`）。原因是 `tools/mesa/*.patch` 会被 16.5 的 `mk-mesa.sh` 用通配符自动收下，那个 patch 默认开着逐帧计时，会把测量开销带进 16.5 的库，而所有性能对比都以 16.5 为参照。删除后 `mesa_from_stamp` 在本目录上的输入是 8 个 `tools/mesa/*.patch`（`air-frontend`、`air-ir3`、`air-metal`、`freedreno-darwin`、`ir3-round-default`、`ir3-shift-split`、`ir3-ssbo-subdword`、`osmesa-es-profile`）加 `rebase-26/` 的 4 个 patch、`redwood-drm/` 的 3 个文件，再加末尾一行版本号；以 `mesa_from_stamp . 26.2.5 26` 复现，戳为 `b24690de…`。

同一个目录上还有第二个戳，两个别混：

| 戳 | 算法 | 谁写、谁读 | 输入集合 |
|---|---|---|---|
| `mesa-$VERSION/.redwood-patches` | `mk-mesa.sh:99` 的 `cat tools/mesa/*.patch $REBASE_PATCHES \| sha256sum`，`$REBASE_PATCHES` 只在 `REBASE=26` 时是 `rebase-26/*.patch`；`:100` 比对，`:128` 写入 | `mk-mesa.sh` 自己写自己读 | patch 的**内容**，不含 `redwood-drm/`，不含版本号行。本目录上 `REBASE` 未设时是 `6a001314…` |
| `out/built-from` | `tools/mesa-stamp.sh:26-35` 的 `mesa_from_stamp`：三个 `find`（`tools/mesa/*.patch`、`rebase-26/*.patch` 当 `REBASE=26`、`redwood-drm/` 全部文件）出文件名，`LC_ALL=C sort \| xargs cat`，再追加一行 `mesa-$VERSION REBASE=$REBASE`，整体 `sha256sum` | `mk-mesa.sh:256` 写，`userland/build.sh:339` 读（`mesa_from_stamp .. 26.2.5 26`） | 上面三组文件的内容**加**版本号与 `REBASE` 那一行，所以同一个目录换 `VERSION` 或 `REBASE` 就会换戳 |

第三个戳 `out/built-from-air`（`mesa_air_stamp`，`mk-mesa.sh:225`）只哈希 `tools/mesa/air/`，与上面两个都不重叠。`air/` 这个子目录进不了前两个戳：`mesa_from_stamp` 的三个 `find` 都带 `-maxdepth 1`，`.redwood-patches` 用的是不递归的通配符 `tools/mesa/*.patch`。而 `draw-perf.patch` 当时直接在 `tools/mesa/` 下，两个戳都会收它，这才是它必须删掉的原因。

`6a001314…` 这个值在本工作区任何一份戳文件里都不存在：现存的 `.redwood-patches` 是内核树那三份（`hackios-xnu-kernel/out/ios26-resume/mesa-arm64e/mesa-26.2.5/` 是 `216c4533…`、`hackios-xnu-kernel/out/turnip-repro/xnu-deps/build/mesa/mesa-26.2.5/` 是 `b4aa4945…`、`hackios-xnu-kernel/xnu-deps/build/mesa/mesa-26.2.5/` 是 `3a43c7c4…`），它们哈希的是 `hackios-xnu-kernel/tools/mesa/`，与本目录是两份不同的补丁集。现存的 `built-from` 与 `built-from-air` 各一份，都在 `hackios-xnu-kernel/xnu-deps/build/mesa/out/`，是 `1ea71614…` 与 `93021ae0…`，同样出自内核树那一份。
