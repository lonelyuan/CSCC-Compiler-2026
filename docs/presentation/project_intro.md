---
marp: true
theme: compiler2026
paginate: true
size: 16:9
html: true
title: 编译器驱动的动态算子图并行
description: CSCC Compiler 2026 项目四分钟介绍
---

<!-- _class: cover -->

<div class="eyebrow">CSCC COMPILER 2026</div>

<div class="cover-copy">

# 编译器驱动的<br><span class="blue">动态算子图并行</span>

## 从 LLVM IR 恢复算子语义，用通用运行时释放多核性能

</div>

<div class="hero-grid" aria-hidden="true">
  <div class="trace"></div>
  <div class="tile t1"><strong>LLVM</strong></div>
  <div class="tile t2"><strong>TRSM</strong></div>
  <div class="tile t3"><strong>MADD</strong></div>
  <div class="tile t4"><strong>RUNTIME</strong></div>
</div>

<div class="cover-meta">动态算子图编译与并行调度｜作品介绍</div>

<!--
大家好，本项目面向动态算子图编译与并行调度赛题。在不修改官方分块 Cholesky 算法和算子的前提下，我们用 LLVM Pass 与通用运行时释放隐藏的并行性。下面介绍方案、四个瓶颈和当前结果。

[Sources]
- /Users/null/Projects/CSCC-Compiler-2026/docs/technical_report.tex
[/Sources]
-->

---

<div class="slide-head">
  <h1>串行循环里，隐藏着一张动态算子图</h1>
  <div class="kicker">真正的难点是恢复依赖，而不是简单开线程</div>
</div>

<div class="two-col">
  <div class="matrix-wrap">
    <div class="matrix" aria-label="分块 Cholesky 下三角算子图示意">
      <div class="cell chol">CHOL</div><div class="cell blank"></div><div class="cell blank"></div>
      <div class="cell trsm">TRSM</div><div class="cell chol">CHOL</div><div class="cell blank"></div>
      <div class="cell madd">MADD</div><div class="cell trsm">TRSM</div><div class="cell chol">CHOL</div>
    </div>
  </div>
  <div class="fact-list">
    <div class="fact"><div class="num">1</div><div><strong>算子依赖随输入变化</strong><p>CHOL、TRSM、MADD 的任务数量和图形由 n、b 共同决定。</p></div></div>
    <div class="fact"><div class="num">2</div><div><strong>低层 IR 丢失显式坐标</strong><p>优化后块地址可能折叠进 GEP、PHI 与指针差表达式。</p></div></div>
    <div class="fact"><div class="num">3</div><div><strong>并行必须守住规则边界</strong><p>保留官方 ABI、数值顺序与 verifier，不把算法搬进 runtime。</p></div></div>
    <div class="callout">目标：让编译器识别语义，让运行时只负责执行。</div>
  </div>
</div>

<!--
分块 Cholesky 表面上是循环，实际是 CHOL、TRSM、MADD 组成的动态图：CHOL 推进 panel，多个 TRSM 和后续 MADD 可以并行。但图形随输入变化，优化后坐标又藏进 GEP 和 PHI，同时还必须保留官方 ABI。因此核心不是简单开线程，而是从 IR 恢复可信的任务语义。

[Sources]
- /Users/null/Projects/CSCC-Compiler-2026/docs/technical_report.tex
- /Users/null/Projects/CSCC-Compiler-2026/docs/technical_scheme_notes.md
[/Sources]
-->

---

<div class="slide-head">
  <h1>Pass 生成任务，Runtime 管理区间</h1>
  <div class="kicker">两阶段调度 + 持久无锁 phase</div>
</div>

<div class="pipeline current">
  <div class="pipe-box"><strong>官方 baseline</strong><span>算法与算子 ABI 保持不变</span></div>
  <div class="pipe-arrow">→</div>
  <div class="pipe-box accent"><strong>LLVM Pass</strong><span>识别、克隆、outline range</span></div>
  <div class="pipe-arrow">→</div>
  <div class="pipe-box"><strong>同步 CHOL</strong><span>确定当前 panel 起点</span></div>
  <div class="pipe-arrow">→</div>
  <div class="pipe-box"><strong>TRSM range</strong><span>一次提交覆盖整列</span></div>
</div>

<div class="phase-ribbon">
  <span>wait</span><b>→</b><span class="active">二维 MADD range</span><b>→</b><span>panel wait</span>
</div>

<div class="principles">
  <div class="principle"><strong>Pass 保留官方调用</strong>任务函数中仍直接调用 trsm 与 madd。</div>
  <div class="principle"><strong>Runtime 保持通用</strong>只接收函数指针、context 和整数区间。</div>
  <div class="principle"><strong>短 phase 无锁发布</strong>epoch session 避免反复经过共享队列。</div>
</div>

<!--
LLVM Pass 识别目标函数并把循环 outline 成 range 任务。当前每个 panel 先同步 CHOL，再提交一次 TRSM range，等待后提交二维 MADD range。短 phase 由持久 epoch session 发布。任务内仍直接调用官方 trsm 和 madd，运行时只管理函数指针、上下文和整数区间。

[Sources]
- /Users/null/Projects/CSCC-Compiler-2026/docs/technical_report.tex
- /Users/null/Projects/CSCC-Compiler-2026/submission/pass/dag_pass.cpp
- /Users/null/Projects/CSCC-Compiler-2026/submission/runtime/dag_runtime.cpp
[/Sources]
-->

---

<div class="slide-head">
  <h1>四个瓶颈，决定优化主线</h1>
  <div class="kicker">profile、对照实验和全量 CSV 决定每一步</div>
</div>

<div class="bottleneck-grid">
  <div class="bottle">
    <span class="bottle-no">01</span>
    <h2>细粒度 DAG 元数据</h2>
    <p><b>证据：</b>n=1152、b=16 时固定任务达 64,752 个，提交线程供给不足。</p>
    <div class="answer">两阶段屏障 → 持久无锁 phase</div>
  </div>
  <div class="bottle">
    <span class="bottle-no">02</span>
    <h2>粒度与平台不匹配</h2>
    <p><b>证据：</b>Xeon 的 50,000 FLOP 粒度迁移到 AArch64 后过细。</p>
    <div class="answer">目标平台重标定到 200,000 FLOP</div>
  </div>
  <div class="bottle">
    <span class="bottle-no">03</span>
    <h2>小 tile 访存复用不足</h2>
    <p><b>证据：</b>b=8 从 8 核 3.379× 降到 40 核 2.827×。</p>
    <div class="answer">二维 (row group, col group) MADD tiling</div>
  </div>
  <div class="bottle">
    <span class="bottle-no">04</span>
    <h2>逐行 TRSM 固定延迟</h2>
    <p><b>证据：</b>MADD 变快后，逐行 context 与 submit 进入关键路径。</p>
    <div class="answer">每个 panel 只提交一次 TRSM range</div>
  </div>
</div>

<!--
主线解决了四类成本。第一，细粒度 DAG 产生六万多个任务，提交端先被元数据拖住，因此改用 phase。第二，Xeon 上五万 FLOP 的粒度到 AArch64 后过细，重新标定为二十万。第三，小 tile 加核反而变慢，因此加入二维 MADD tiling 提高复用。第四，MADD 变快后，逐行 TRSM 提交进入关键路径，于是每个 panel 只提交一次 range。

[Sources]
- /Users/null/Projects/CSCC-Compiler-2026/docs/technical_report.tex
- /Users/null/Projects/CSCC-Compiler-2026/docs/engineering_log.md
- /Users/null/Projects/CSCC-Compiler-2026/docs/benchmark_results/r16_exec_attrib.csv
[/Sources]
-->

---

<div class="slide-head">
  <h1>四次关键迭代，把分数推到 53.85</h1>
  <div class="kicker">40 核 AArch64｜150 个公开用例等权</div>
</div>

<div class="journey">
  <div class="journey-line"></div>
  <div class="milestone">
    <span class="dot"></span><span class="round">ROUND 15</span>
    <strong>48.19</strong><em>4.370×</em>
    <p>平台粒度重标定</p>
  </div>
  <div class="milestone">
    <span class="dot"></span><span class="round">ROUND 17</span>
    <strong>51.48</strong><em>6.123×</em>
    <p>持久无锁 phase</p>
  </div>
  <div class="milestone">
    <span class="dot"></span><span class="round">ROUND 20</span>
    <strong>53.14</strong><em>7.006×</em>
    <p>二维 MADD tiling</p>
  </div>
  <div class="milestone current">
    <span class="dot"></span><span class="round">ROUND 28</span>
    <strong>53.85</strong><em>7.386502×</em>
    <p>TRSM range + HEAD 校准</p>
  </div>
</div>

<div class="journey-note">负结果同样收敛路线：完整跨 panel DAG、priority、共享原子 counter、tile packing 与默认 work stealing 均未进入主线。</div>

<!--
Git 和全量 CSV 记录了连续提升：平台粒度重标定后是四十八点一九；持久无锁 phase 提高到五十一点四八；二维 MADD tiling 提高到五十三点一四；TRSM range 最终把当前结果推到五十三点八五。完整跨 panel DAG、优先级和默认 work stealing 等路线没有稳定收益，因此没有进入主线。

[Sources]
- /Users/null/Projects/CSCC-Compiler-2026/docs/performance.md
- /Users/null/Projects/CSCC-Compiler-2026/docs/technical_report.tex
- /Users/null/Projects/CSCC-Compiler-2026/docs/benchmark_results/r28_head5394c43_cg_baseline.csv
[/Sources]
-->

---

<div class="slide-head">
  <h1>当前主线：7.386502× / 53.85 分</h1>
  <div class="kicker">150/150 PASS｜公开用例本地同口径</div>
</div>

<div class="metric-strip results">
  <div class="metric"><span class="value good">150/150</span><span class="name">Verifier PASS</span><div class="detail">优化前后逐用例数值校验</div></div>
  <div class="metric primary"><span class="value">7.386502×</span><span class="name">几何平均加速比</span><div class="detail">逐 case 等权的正式性能口径</div></div>
  <div class="metric"><span class="value">53.85</span><span class="name">公开用例折算分</span><div class="detail">m<sub>ideal</sub> = 32，功能分 100</div></div>
</div>

<div class="bucket-chart">
  <div class="bucket b1"><span>b &lt; 12</span><i>7.76×</i></div>
  <div class="bucket b2"><span>12 ≤ b &lt; 32</span><i>9.75×</i></div>
  <div class="bucket b3"><span>32 ≤ b &lt; 128</span><i>8.18×</i></div>
  <div class="bucket b4"><span>b ≥ 128</span><i>3.87×</i></div>
</div>

<!--
当前主线在四十核 AArch64 上重测一百五十个公开用例，全部通过 verifier。逐用例等权几何平均加速比为七点三八六五，折算五十三点八五分。中等块收益最高，大块则受可并行块数限制。这里强调：五十三点八五是公开用例本地折算值，不是官方线上成绩。

[Sources]
- /Users/null/Projects/CSCC-Compiler-2026/docs/benchmark_results/r28_head5394c43_cg_baseline.csv
- /Users/null/Projects/CSCC-Compiler-2026/docs/benchmark_results/r28_head5394c43_baseline.log
- /Users/null/Projects/CSCC-Compiler-2026/docs/technical_report.tex
[/Sources]
-->

---

<div class="slide-head">
  <h1>项目亮点，是一条可验证的编译优化闭环</h1>
  <div class="kicker">正确性、合规性与性能证据同步收敛</div>
</div>

<div class="proof-layout">
  <div class="proof-main">
    <div class="proof-row"><span>01</span><div><strong>编译器可信</strong><p>从优化后 IR 恢复块坐标，生成一维/二维语义任务；官方 trsm、madd 调用仍保留在任务内。</p></div></div>
    <div class="proof-row"><span>02</span><div><strong>运行时通用</strong><p>线程池、epoch phase、range 切分和 profile 不理解 Cholesky 算子语义。</p></div></div>
    <div class="proof-row"><span>03</span><div><strong>实验可复现</strong><p>Verifier、IR 断言、受影响组/控制组与全量 CSV 共同决定采纳或回退。</p></div></div>
  </div>
  <div class="next-box">
    <span class="label">NEXT BOTTLENECK</span>
    <h2>继续压缩必要内存流量与每 panel 固定工作</h2>
    <p>重点覆盖小 tile 带宽平台，以及 12 ≤ b &lt; 128 的主要得分区间。</p>
    <div class="target">到 60 分仍需约 <b>1.444×</b> 全量提升</div>
  </div>
</div>

<!--
项目亮点不只是一组分数：编译器从 IR 恢复语义，任务内保留官方调用；运行时保持通用；每次改动都经过 verifier、IR 断言、机制对照和全量 CSV。下一步优先减少小 tile 的必要内存流量，并压缩中等 tile 的每 panel 固定工作。到六十分仍需要约一点四四四倍提升。

[Sources]
- /Users/null/Projects/CSCC-Compiler-2026/docs/technical_report.tex
- /Users/null/Projects/CSCC-Compiler-2026/docs/roadmap.md
- /Users/null/Projects/CSCC-Compiler-2026/submission/pass/dag_pass.cpp
- /Users/null/Projects/CSCC-Compiler-2026/submission/runtime/dag_runtime.cpp
[/Sources]
-->

---

<!-- _class: closing -->

<div class="eyebrow">TAKEAWAY</div>

<div class="closing-title">用编译器恢复语义，<br>用通用运行时兑现并行</div>

<div class="close-stats"><b>150/150 PASS</b><b>7.386502×</b><b>53.85</b></div>
<div class="close-line">正确性可验证｜优化路径可解释｜性能结果可复现</div>

<div class="accent-line"></div>

<!--
总结来说，我们用编译器恢复语义，用通用运行时兑现并行。当前实现一百五十个用例全部正确、七点三八六五倍加速、公开用例折算五十三点八五分，并且路径可解释、结果可复现。谢谢大家。

[Sources]
- /Users/null/Projects/CSCC-Compiler-2026/docs/technical_report.tex
- /Users/null/Projects/CSCC-Compiler-2026/docs/benchmark_results/r28_head5394c43_cg_baseline.csv
[/Sources]
-->
