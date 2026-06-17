# docs/archive — 归档文档

这里放**不再描述当前状态、也不再被活代码/文档引用**的历史文档。它们有历史价值（记录当时
的数据、设计的演变），但日常不需要翻。整理原则见 [`../refs/README.md`](../refs/README.md)。

> 注：`openspec/changes/archive/` 等历史文件里指向这些文档的旧路径（`docs/refs/…`）**没有
> 回改**。按文件名全局搜索即可定位——文件名稳定，路径不再维护。

## benchmarks/ — 早期基准结果快照

某个提交点测得的吞吐数据，已被更新的快照取代。

- `baseline-benchmark-results-2026-05.md` — 最早的 baseline 结果。
- `baseline-benchmark-results-cache-2026-05.md` — cache 轨首份 baseline。
- `baseline-benchmark-results-cache-2026-05-neon-inline.md` — NEON inline 之后的快照。
- `baseline-benchmark-results-cache-2026-05-clearforwrite-helpers.md` — clearforwrite /
  helpers 之后的快照。

## profiles/ — 早期 cache 热路径 profile

`refs/gaby-vm-cache-hotpath-profile-2026-05-27.md`（仍在用的那份）之外的早期/后续采样。

- `gaby-vm-cache-hotpath-profile-2026-05.md` — 最早的热路径 profile。
- `gaby-vm-cache-hotpath-profile-2026-06-02.md` — 后续 profile + 优化 roadmap。（带 `.html`，
  未纳入 git 跟踪）

## superseded/ — 被取代 / 草稿 / 过程记录

- `gaby-vm-vixl-sim-test-port-design-2026-06-08.md` — `vixl_port` 早期「冻结 fixture」设计，
  被 live-assemble 重写取代。
- `gaby-vm-vixl-sim-test-port-plan-2026-06-08.md` — 上者的实现计划，同被取代。
- `gaby-vm-vixl-port-live-assemble-review-2026-06-10.md` — live-assemble 重写的 code review
  过程记录。
- `gaby-vm-dispatch-redesign-notes-2026-06-02.md` — dispatch 重设计的粗记。
- `gaby-vm-modification-sketch.md` — 早期改造草图（总方向）。

## 根目录

- `HANDOFF-predecode-cache-brainstorm.md` — 预解码缓存的早期 brainstorm 交接记录（先于本次
  归档就在此处）。
