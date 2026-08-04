# 平台评测与云桌面操作

本文记录已在 2026-08-05 通过已登录竞赛账号核对的页面行为，供提交前检查和浏览器辅助操作使用。它不是绕过平台鉴权的 API 文档：不要保存或复用 URL 中的一次性参数；RFB 客户端仅在当前进程内解析连接能力，不打印其内容。没有明确测试指令时不得上传或触发评测。

## 提交前自动化边界

可自动化的部分是本地/VM 的构建、正确性、基准测试、打包、压缩包结构检查，以及用户自有云桌面中的上传和测试执行。对云桌面，明确的“运行测试”指令加上一条新鲜认证链接即为一次性授权；正式竞赛提交仍是独立操作，不能由本流程触发。

推荐流水线：

```text
sync -> build -> smoke verifier -> benchmark + CSV -> package
     -> unzip/CMake 复建检查 -> 浏览器上传 -> 云桌面执行 -> 读取结构化结果
```

先在本地工作树同步：

```bash
./scripts/sync_to_vm.sh
```

再在 openEuler/BiSheng VM 的 `/root/bisheng` 中执行：

```bash
source /etc/profile.d/bisheng.sh
cd /root/bisheng
./submission/scripts/build.sh
SPEC_START=91 SPEC_END=96 COMPILER2026_DAG_THREADS=4 \
  ./submission/scripts/smoke_test.sh
LABEL=pre_submit REPEAT=3 COMPILER2026_DAG_THREADS=4 \
  ./submission/scripts/benchmark.sh
./submission/scripts/package.sh
sha256sum dist/submission.zip
```

打包后应在干净目录复建，确保 archive 根目录直接包含 `CMakeLists.txt`：

```bash
rm -rf /tmp/judge_zip_test
mkdir -p /tmp/judge_zip_test/submission /tmp/judge_zip_test/build
unzip -q /root/bisheng/dist/submission.zip -d /tmp/judge_zip_test/submission
cmake -S /tmp/judge_zip_test/submission -B /tmp/judge_zip_test/build -G Ninja \
  -DLLVM_CONFIG=/opt/bisheng/bin/llvm-config \
  -DCMAKE_C_COMPILER=/opt/bisheng/bin/clang \
  -DCMAKE_CXX_COMPILER=/opt/bisheng/bin/clang++
cmake --build /tmp/judge_zip_test/build -j"$(nproc)"
```

自动化报告至少应显示：提交包路径、SHA-256、文件大小、smoke verifier 结果、benchmark CSV 路径和与上次提交的性能比较。任何一项失败时停止，不上传。

## 已核对的网页上传请求

当前赛题的题目页由 `assignID=47609`、`problemID=3153371` 标识；这些是当前页面事实，平台后续可能调整，脚本必须从已登录页面读取而非硬编码为长期契约。

- 入口：`/assignment/programOJPList.jsp?proNum=1&assignID=47609`
- 允许文件：`.zip` 或 `.rar`，页面限制为 `102400000` bytes。
- 上传请求：`POST /assignment/showOJPProcessMsg.jsp?problemID=3153371&assignID=47609&doSubmit=true&wtime=<elapsed-seconds>`。
- 请求体：`multipart/form-data`，文件字段名为 `file`。小型非压缩文件可能附加 `fileEncoding=gzip`；正常提交包为 zip，应保持原始压缩内容。
- 成功上传后：进入 `showOJPProcessMsg.jsp?...&doNewSubmit=true` 查看队列、构建、功能和性能评测输出。

这些请求依赖浏览器的已登录会话和页面生成的短期状态。不要直接用命令行重放请求，也不要从 URL、脚本或浏览器存储中提取凭证。正式竞赛提交必须单独显式授权，且上传前至少确认以下内容：

1. 文件确为刚刚通过复建检查的 `dist/submission.zip`。
2. 所见 SHA-256 与预览一致。
3. 当前页面仍是目标赛题，且未处于其他队伍/题目的提交页。
4. 操作者明确同意开始一次新的线上评测。

评测结束后，保存页面显示的功能通过数、几何平均加速比、总分以及完整的 `judge_stdout_tail` 到工程日志；不要把网页上的临时下载链接或参数提交进 Git。

## 云桌面：适合什么，不适合什么

在线 IDE 是 noVNC WebSocket 会话，而不是稳定的 SSH 主机。入口 URL 的 `desktopParam` 与 `cicdParam` 会随会话轮换；它们不应被记录、共享或用作长期连接凭证。

已公开的工作流能力：

- GUI 终端和桌面操作；
- 剪贴板；
- 远程桌面文件上传/下载；
- `/mnt/cgshare` 工作目录，以及网页中的工作目录文件浏览器。

当前界面未公开 SSH、端口转发或 VS Code Remote-SSH 入口。因此将它作为“最后一公里”的目标环境：运行官方环境特有的构建/验收、保存日志到 `/mnt/cgshare`、再拿回本地分析；不要把日常编辑、代码审阅或大规模实验迁入 VNC。

如果赛方将来提供明确的 SSH 或受支持的端口转发，再建立独立的 VS Code Remote-SSH/调试配置。此前不得通过猜测端口、复用 noVNC 参数或修改桌面网络配置来尝试绕过平台边界。

### 文件上传与结果读取接口

已核对的“更多 → 上传文件至远程桌面”对话框是一个独立 iframe。它是可以由浏览器 agent 稳定定位的 DOM 表单，不需要坐标点击：

- 当前提交目标：`POST /authincludes/expEnv/doexpDeskDockerUpload.jsp?doUpload=true&cicdParam=<由当前页面生成的短期值>`。
- 请求体：`multipart/form-data`，实际文件字段为 `file`。页面原生 `<input>` 的名称是 `uploadname`，但其 JavaScript 会构造 `FormData` 并以 `file` 发送。
- 限制：64 MiB；文件名不得含中文或空格；成功文件写入 `/mnt/cgshare`。
- 完成信号：上传成功或服务端错误文本会写入 iframe 中的 `#uploadTipMSG`；进度条为 `#localfileprocess`。网络失败没有完整的前端失败回调，因此 agent 应设置超时并报告未确认，而非假定成功。

短期 `cicdParam` 是会话凭证的一部分，不能记录、缓存或在后续会话中重放。自动化应从刚打开、已登录的云桌面页面读取 iframe 的当前 `form[action]`，或者直接通过页面的 file input 和上传按钮完成提交。

对于用户明确提供的新鲜认证链接的云桌面测试，可以在当前运行中从该 URL 提取 `cicdParam` 并用 `curl -F file=@...` 发送同一请求；这不是长期 API，也不能缓存参数或复用于正式竞赛提交。仓库的 `scripts/upload_cloud_desktop_bundle.sh` 封装该操作，刻意不打印或写入认证 URL。

2026-08-05 实测还确认了一个关键条件：只带 `cicdParam` 的匿名 curl 可能返回空的 HTTP 200，但文件不会出现在 `/mnt/cgshare`；加入与页面相同的 XHR 请求头后会收到 302。上传端点因此还依赖同一份 CourseGrading 登录会话 Cookie。脚本支持由操作者显式提供 Netscape 格式 Cookie 文件：

```bash
CLOUD_DESKTOP_COOKIE_FILE=/secure/path/course.cookies.txt \
  ./scripts/upload_cloud_desktop_bundle.sh '<fresh-desktop-url>' '<bundle.zip>'
```

Cookie 文件不得放入仓库、日志或命令示例；仓库忽略本地文件名 `.cloud-desktop-cookies.txt`。脚本把非 2xx 和空响应都视为失败。最终仍必须在远端确认文件存在，不能仅以 HTTP 状态判断成功。

可固化为浏览器 agent 的状态机：

```text
open authenticated desktop URL
  -> wait for desktop page
  -> “更多” -> “上传文件至远程桌面”
  -> frame[name=uploadFRAME] appears
  -> verify local file: name/size/SHA-256
  -> confirm an explicit cloud-desktop test instruction is in scope
  -> set file on #uploadname
  -> click #uploadsubmitBtn
  -> wait for #uploadTipMSG (or timeout/failure)
  -> verify expected file name under /mnt/cgshare before execution
```

前六步可使用页面语义定位器。对于云桌面测试，用户提供的明确运行指令和新鲜认证 URL 已授权 `set file` 与点击“上传”；对于正式竞赛提交，仍必须单独确认。不要把点击路径写成固定坐标脚本。

当前 Chrome agent 对操作系统文件选择框调用 `setFiles` 会返回 `Not allowed`，因此不能依赖原生上传对话框实现无人值守。对已授权的云桌面测试，只有在显式提供匹配登录 Cookie 文件时才使用一次性 curl 上传器；否则上传仍需要操作者在系统文件选择框中选择文件。浏览器继续负责 noVNC 命令执行和结果读取。

### 剪贴板、键盘和 RFB 接口

页面的英文剪贴板不是操作系统文件选择框，它是稳定的 DOM/RFB 桥：

```text
textarea#clipboardText
  -> button#clipboardBtn
  -> rfb.clipboardPasteFrom(text)
  -> RFB ClientCutText
  -> 远端 X11 剪贴板
```

noVNC 内的终端不是 DOM。终端已经最大化时，已实际验证的最小序列是：

1. 把命令写入 `#clipboardText` 并点击 `#clipboardBtn`。
2. 点击一次 Canvas 内终端输入区，发送 `Ctrl+U` 清理可能残留的半行命令。
3. 在终端内右键，选择 `Paste`，发送 `Enter`。

这里的右键菜单属于远端 XFCE Terminal，不能用页面 DOM 定位；坐标应由当前 Canvas 矩形和菜单相对偏移计算，不能硬编码屏幕绝对坐标。准备脚本生成单行 `remote-command.txt`，使用 `python3 -m zipfile` 解包，因为该云镜像实测有 Python 3、没有 `unzip`。

页面初始化代码还公开了底层协议结构：

```text
wss://course.educg.net/websockify?token=<desktopParam 中的短期 token>
RFB 3.8 + VNC Authentication（password 同样来自 desktopParam）
```

因此可以跳过 Canvas，直接发送 RFB PointerEvent、ClientCutText 和 KeyEvent。`vncdotool` 默认连接裸 TCP VNC，不能直接处理这里的 WebSocket transport；仓库提供最小的 RFB-over-WebSocket 客户端 `scripts/cloud_desktop_rfb.py`。安装隔离依赖：

```bash
uv venv build/cloud_desktop/.venv --python 3.12
uv pip install --python build/cloud_desktop/.venv/bin/python \
  -r requirements-cloud-desktop.txt
```

连接探测：

```bash
build/cloud_desktop/.venv/bin/python scripts/cloud_desktop_rfb.py \
  '<fresh-desktop-url>' \
  --cookie-file /secure/path/course.cookies.txt \
  probe
```

发送生成的命令并等待 `result.env` 通过终端全选/复制和 RFB ServerCutText 返回：

```bash
build/cloud_desktop/.venv/bin/python scripts/cloud_desktop_rfb.py \
  '<fresh-desktop-url>' \
  --cookie-file /secure/path/course.cookies.txt \
  send \
  --command-file build/cloud_desktop/cloud_eval_<label>/remote-command.txt \
  --result-file /mnt/cgshare/cloud_eval_<label>/result.env
```

只有 `cicdParam` 的短链接需要先在同一登录会话中换取 `desktopParam`。实测无 Cookie 的直接 WebSocket Upgrade 会超时，因此“新鲜 URL 本身”目前不足以建立独立 RFB 客户端。客户端不会打印或保存 token/password。提供匹配 Cookie 后，WebSocket Upgrade、RFB 3.8/VNC Authentication、ClientCutText、KeyEvent 和 ServerCutText 结果回传均已端到端验证。

远端镜像实测没有 `xclip` 和 `xsel`。结果读取不依赖这两个工具：runner 完成后清屏并打印 `CLOUD_EVAL_RESULT ... CLOUD_EVAL_RESULT_END`，自动化相对 Canvas 左上角操作最大化 XFCE Terminal 的 `Edit → Select All → Edit → Copy`。页面 `#clipboardText` 已实际读回完整探针结果；纯 RFB 客户端则等待对应 ServerCutText。该菜单坐标只在“终端最大化、默认菜单栏可见”这一预条件下使用。

每次命令应先写入 `/mnt/cgshare/run-<sha>/result.txt` 或 `result.env`，再通过以下优先顺序读取：

1. 远程剪贴板回传（页面 `#clipboardText` 可读，英文最多 4096 字符）；
2. 工作目录文件浏览器或“下载远程桌面内的文件”；
3. 仅在前两者不可用时，对终端截图/OCR。

推荐远程命令始终输出机器可读的短结果，例如 `status=<...>`、`exit_code=<...>`、`log=/mnt/cgshare/...`。不要让 agent 从长终端滚屏中推断成败。

### 当前云桌面镜像的构建能力

2026-08-05 对当前会话实测：测试包通过带登录 Cookie 的 curl 成功上传，远端 Python 3 可以解包，RFB 客户端可以执行命令并读回唯一标记的结果。但镜像中没有 `/etc/profile.d/bisheng.sh`、`/opt/bisheng`、clang、llvm-config、cmake 或 ninja，只有系统 GCC；仓库 SDK 的预编译 generator/verifier 又是 x86-64，不能用于该 aarch64 桌面。因此当前会话返回 `status=setup_error, exit_code=127`，不能运行 LLVM smoke test。

这属于目标环境缺少构建前提，不是上传/RFB 自动化失败。`cloud_desktop_run.sh` 会检查必需工具并始终写出结构化 `result.env`，不得把该结果报告为测试通过。

## 推荐的 AI 协作分层

1. **本地仓库为主**：AI 直接读取、修改、测试 Pass/runtime/doc，维护 Git 历史和 benchmark CSV。
2. **现有 openEuler VM 为兼容性闸**：通过 `scripts/sync_to_vm.sh` 做 BiSheng 构建、smoke 和可复现实验；它已经是可脚本化的远程开发环境。
3. **云桌面只做赛方环境闸**：使用 `/mnt/cgshare` 交换一个小而明确的 bundle（提交包、日志、CSV），不在其中长期维护工作树。
4. **浏览器仅负责需要登录态的动作**：读取评测、准备上传、在人工确认后上传和轮询；浏览器不持久化认证材料。

这样把 AI 最擅长的代码/实验闭环放在可复现的本地与 VM，把难以自动化的 GUI 会话压缩为少量、可审计的人工节点。
