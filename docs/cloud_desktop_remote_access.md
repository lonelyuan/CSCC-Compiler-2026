# 云桌面 Tailscale 组网与 SSH 接入

本文记录 CourseGrading AArch64 云桌面的远程接入方案。2026-08-05，本项目已就
“使用自有组网方式建立远程接入”向比赛组委会咨询并获得许可；这项许可是本项目的
操作前提，不应推断为平台对所有参赛者的通用授权。

## 已验证环境

- openEuler 22.03 LTS，`aarch64`；
- 40 个在线 CPU，单线程/核、单 NUMA 节点；
- 75 GiB 内存，`/mnt/cgshare` 与根文件系统约有 325 GiB 可用空间；
- 容器没有 `/dev/net/tun`，不能使用 Tailscale 内核 TUN 模式；
- 出站 HTTPS 正常，系统已有 `/usr/sbin/sshd`；
- `tailscaled --tun=userspace-networking` 可以登录 Tailnet；
- 当前网络可能只建立 DERP 中继连接。中继延迟影响交互体验，但不影响远端本机的
  核心计算计时；跑 benchmark 时仍应避免并发上传和下载。

验证成功的链路是：

```text
本地 OpenSSH
  -> Tailnet（可能经 DERP）
  -> userspace tailscaled
  -> Tailscale TCP Serve :22
  -> 127.0.0.1:2222
  -> 独立 sshd（仅公钥认证）
```

不要在该容器中使用内置 Tailscale SSH 作为最终入口。实测 `tailscale up --ssh` 能让
Tailnet 建立连接，但 SSH 客户端在发送本地 banner 后收不到服务端 banner。标准
`sshd` 加 TCP Serve 已端到端验证通过。

## 测评环境边界

- 原 4-vCPU AArch64 VM 已退役，不再作为兼容性闸。
- `43.142.45.204:6000` 是 Ubuntu x86_64 Xeon 多核调度实验机，只有核心数规模接近评测
  环境，不提供 AArch64、openEuler、BiSheng 或跨机绝对性能等价性。
- 本云桌面是当前唯一现役 AArch64 远程环境，也是最接近正式环境的候选；在完成同源、同
  输入、同计时边界和逐 case 等权几何平均对照前，不能声称与正式 judge 一致。
- 正式结果校准目标为 `audit 150/150`、`perf 150/150`、等权 per-case geomean
  `2.393844`、`m_ideal=32.0`。SSH 是否经 DERP 不影响远端进程内计时，但跑测时必须停止
  同步和下载。

## 安全边界

- Tailscale 节点状态、日志、host key 和 SSH 公钥配置只放在持久目录
  `/mnt/cgshare/tailscale-cloud`，不得提交到 Git 或复制进测试 bundle。
- 不使用 Tailscale Funnel；TCP Serve 仅对 Tailnet 可见。
- `sshd` 只监听 `127.0.0.1:2222`，关闭密码、PAM、交互式认证和 root 密码登录。
- Tailnet 登录 URL、auth key、节点 state 和私钥都是认证材料，不得写入文档或日志。
- 文档不固化 Tailnet IP、个人公钥或 Tailnet 域名。连接时优先使用 MagicDNS 主机名
  `bisheng-cg-aarch64`。
- 以下命令特意不使用 `set -e`，避免一次普通失败直接关闭云桌面的交互式终端。

## 首次引导

首次操作仍通过已登录的 noVNC 云桌面终端完成。

### 1. 安装固定版本的 Tailscale

```bash
cd /mnt/cgshare
TS_ROOT=/mnt/cgshare/tailscale-cloud
TS_VERSION=1.102.2
TS_DIR=tailscale_${TS_VERSION}_arm64
TS_ARCHIVE=$TS_ROOT/tailscale_${TS_VERSION}_arm64.tgz

mkdir -p "$TS_ROOT/pkg" "$TS_ROOT/run" "$TS_ROOT/state" "$TS_ROOT/ssh"
chmod 700 "$TS_ROOT" "$TS_ROOT/run" "$TS_ROOT/state" "$TS_ROOT/ssh"

curl -fL --retry 5 --retry-delay 3 \
  -o "$TS_ARCHIVE" \
  "https://pkgs.tailscale.com/stable/tailscale_${TS_VERSION}_arm64.tgz"

printf '%s  %s\n' \
  '2b64e9ade7e73034b5ec9e9bcd537f5ddd14ae3abb435e57e929e7486ae42660' \
  "$TS_ARCHIVE" | sha256sum -c -

tar -xzf "$TS_ARCHIVE" -C "$TS_ROOT/pkg"
"$TS_ROOT/pkg/$TS_DIR/tailscale" version
```

校验必须输出 `OK`，版本必须为 `1.102.2`。升级版本时同时更新下载 URL、目录名和
SHA-256，不能继续复用旧校验值。

### 2. 启动 userspace daemon

```bash
TS_ROOT=/mnt/cgshare/tailscale-cloud
TS_BIN=$TS_ROOT/pkg/tailscale_1.102.2_arm64
TS_SOCKET=$TS_ROOT/run/tailscaled.sock

nohup "$TS_BIN/tailscaled" \
  --tun=userspace-networking \
  --state="$TS_ROOT/state/tailscaled.state" \
  --socket="$TS_SOCKET" \
  --port=0 \
  >"$TS_ROOT/tailscaled.log" 2>&1 &

echo $! > "$TS_ROOT/run/tailscaled.pid"
sleep 3
tail -n 20 "$TS_ROOT/tailscaled.log"
```

首次出现 `NeedsLogin` 和 `Tailscale is stopped` 是未登录状态，不代表 daemon 启动失败。

### 3. 登录 Tailnet

```bash
TS_ROOT=/mnt/cgshare/tailscale-cloud
TS_BIN=$TS_ROOT/pkg/tailscale_1.102.2_arm64
TS_SOCKET=$TS_ROOT/run/tailscaled.sock

"$TS_BIN/tailscale" --socket="$TS_SOCKET" up \
  --hostname=bisheng-cg-aarch64 \
  --accept-dns=false \
  --accept-routes=false
```

在操作者自己的浏览器中打开命令显示的一次性登录 URL 并批准节点。不要复制或发送该
URL。完成后验证：

```bash
"$TS_BIN/tailscale" --socket="$TS_SOCKET" status
"$TS_BIN/tailscale" --socket="$TS_SOCKET" ip -4
```

### 4. 配置独立 sshd

先在本机读取要授权的公钥：

```bash
cat ~/.ssh/id_ed25519.pub
```

在云桌面中把下面占位符替换为完整的单行公钥。只粘贴 `.pub` 公钥，绝不能上传私钥。

```bash
TS_ROOT=/mnt/cgshare/tailscale-cloud
AUTHORIZED_KEY='<粘贴本机的单行 SSH 公钥>'

printf '%s\n' "$AUTHORIZED_KEY" > "$TS_ROOT/ssh/authorized_keys"
unset AUTHORIZED_KEY
chmod 600 "$TS_ROOT/ssh/authorized_keys"

if [[ ! -s "$TS_ROOT/ssh/ssh_host_ed25519_key" ]]; then
  ssh-keygen -q -t ed25519 -N '' -f "$TS_ROOT/ssh/ssh_host_ed25519_key"
fi
chmod 600 "$TS_ROOT/ssh/ssh_host_ed25519_key"
```

生成独立配置：

```bash
TS_ROOT=/mnt/cgshare/tailscale-cloud

printf '%s\n' \
  'Port 2222' \
  'ListenAddress 127.0.0.1' \
  "HostKey $TS_ROOT/ssh/ssh_host_ed25519_key" \
  "PidFile $TS_ROOT/run/sshd.pid" \
  "AuthorizedKeysFile $TS_ROOT/ssh/authorized_keys" \
  'PermitRootLogin prohibit-password' \
  'PubkeyAuthentication yes' \
  'PasswordAuthentication no' \
  'KbdInteractiveAuthentication no' \
  'ChallengeResponseAuthentication no' \
  'PermitEmptyPasswords no' \
  'UsePAM no' \
  'StrictModes no' \
  'AllowUsers root' \
  'UseDNS no' \
  'X11Forwarding no' \
  'Subsystem sftp internal-sftp' \
  > "$TS_ROOT/ssh/sshd_config"

/usr/sbin/sshd -t -f "$TS_ROOT/ssh/sshd_config"
echo "sshd_config_status=$?"
```

只有 `sshd_config_status=0` 才启动：

```bash
TS_ROOT=/mnt/cgshare/tailscale-cloud

if [[ -r "$TS_ROOT/run/sshd.pid" ]] && \
   kill -0 "$(cat "$TS_ROOT/run/sshd.pid")" 2>/dev/null; then
  echo 'sshd already running'
else
  /usr/sbin/sshd \
    -f "$TS_ROOT/ssh/sshd_config" \
    -E "$TS_ROOT/sshd.log"
fi

ssh-keyscan -T 3 -p 2222 127.0.0.1
```

`ssh-keyscan` 必须返回服务端公钥；否则先检查 `$TS_ROOT/sshd.log`。

### 5. 用 TCP Serve 暴露 SSH

```bash
TS_ROOT=/mnt/cgshare/tailscale-cloud
TS_BIN=$TS_ROOT/pkg/tailscale_1.102.2_arm64
TS_SOCKET=$TS_ROOT/run/tailscaled.sock

"$TS_BIN/tailscale" --socket="$TS_SOCKET" set --ssh=false
"$TS_BIN/tailscale" --socket="$TS_SOCKET" serve \
  --bg --tcp=22 tcp://127.0.0.1:2222
"$TS_BIN/tailscale" --socket="$TS_SOCKET" serve status
```

首次使用 Serve 可能要求在浏览器中确认一次。只启用 `serve`，不要启用公开互联网可访问的
`funnel`。

## 从本机连接与同步

仓库提供无个人 IP 和无密钥内容的包装脚本：

```bash
./scripts/cloud_desktop_ssh.sh --check
./scripts/cloud_desktop_ssh.sh
./scripts/sync_to_cloud_desktop.sh
```

默认使用 MagicDNS 名称 `bisheng-cg-aarch64`、本机 `~/.ssh/id_ed25519` 和远端镜像
`/mnt/cgshare/bisheng`。需要覆盖时使用脚本帮助中列出的环境变量。同步默认不删除远端文件；
只有显式设置 `CLOUD_DESKTOP_SYNC_DELETE=1` 才清理远端非排除文件。同步完成后，脚本会在
远端 `.evaluation/source-state.env` 记录本地 commit、dirty 状态和 UTC 同步时间；dirty 工作树
不能只用 commit ID 描述。可先用 `--dry-run` 预览，此时不写源码或元数据。当前基础镜像没有
`rsync`，脚本会自动改用 tar-over-SSH；该后端不会删除旧文件。如果显式请求删除但远端仍无
`rsync`，脚本会拒绝执行。

也可以直接连接：

```bash
ssh -o IdentitiesOnly=yes -i ~/.ssh/id_ed25519 root@bisheng-cg-aarch64
```

如果 `tailscale ping` 只能走 DERP，可以继续使用；它只说明无法建立点对点直连。SSH/SCP
交互会更慢，但 benchmark 在远端本机执行，不应使用网络墙钟作为性能数据。

## 会话恢复与诊断

云桌面容器重启后，`/mnt/cgshare` 中的状态会保留，但进程不会保留。重新执行 userspace
daemon 启动命令，再启动独立 `sshd`，最后检查：

```bash
TS_ROOT=/mnt/cgshare/tailscale-cloud
TS_BIN=$TS_ROOT/pkg/tailscale_1.102.2_arm64
TS_SOCKET=$TS_ROOT/run/tailscaled.sock

"$TS_BIN/tailscale" --socket="$TS_SOCKET" status
"$TS_BIN/tailscale" --socket="$TS_SOCKET" serve status
ssh-keyscan -T 3 -p 2222 127.0.0.1
```

若 Serve 配置未恢复，重新执行 `serve --bg --tcp=22 ...`。若本地 SSH 停在
`Local version string` 后面，依次检查本地 `ssh-keyscan`、Serve 状态和
`tailscaled.log`；不要重新打开内置 Tailscale SSH。

## 停用与撤销

测试完成后可撤销 Tailnet 节点并停止两个 daemon：

```bash
TS_ROOT=/mnt/cgshare/tailscale-cloud
TS_BIN=$TS_ROOT/pkg/tailscale_1.102.2_arm64
TS_SOCKET=$TS_ROOT/run/tailscaled.sock

"$TS_BIN/tailscale" --socket="$TS_SOCKET" serve reset
"$TS_BIN/tailscale" --socket="$TS_SOCKET" logout

if [[ -r "$TS_ROOT/run/sshd.pid" ]]; then
  kill "$(cat "$TS_ROOT/run/sshd.pid")" 2>/dev/null || true
fi
if [[ -r "$TS_ROOT/run/tailscaled.pid" ]]; then
  kill "$(cat "$TS_ROOT/run/tailscaled.pid")" 2>/dev/null || true
fi
```

如果不再使用该节点，还应在 Tailscale 管理台删除它。节点 state 包含认证材料；不要把
`tailscale-cloud` 目录打包、同步或纳入版本控制。
