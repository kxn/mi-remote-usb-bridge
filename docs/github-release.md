# GitHub 固件发版

进入仓库 **Actions → Release firmware → Run workflow**，选择分支并填写：

- `version`：可留空，或填 `0.6.14` / `v0.6.14` / `0.6.15-rc.1`。
- `mode`：`draft` 创建草稿；`publish` 直接发布；`build-only` 仅生成 Actions 附件。
- `prerelease`：标记预发布；带 `-rc.1` 等后缀的版本自动标记。

留空时，以 `firmware/Makefile` 的默认版本为起点；如果 GitHub 已有同版本或更高的正式
三段版本号，取最高版本并将末位加一。标签和 Release（包括草稿）都计入；例如源码
默认 `0.6.14`、已占用 `v0.6.15`，自动得到 `0.6.16`。仅构建不会占用版本号。
工作流串行运行，避免两个自动发版分配同一版本。手工版本冲突直接失败，不覆盖旧标签/附件。

## 构建内容

先跑 C、Python 和模拟器回归，再编译 CH582F：`DEBUG=0`、`SDK_RX_PROBE=0`、
`HOST_VOICE_START=0`、MTU 23。输入版本写入固件，标签为 `v<版本>`，精确指向本次
执行的源码提交，不随构建期间分支更新而移动。不会修改源码默认版本或自动提交文件。

附件包括 `.bin`、Intel `.hex`、ZIP、`manifest.json` 和 `SHA256SUMS.txt`。
ZIP 另含 ELF/MAP、编译选项、工具链/SDK 锁定信息及许可证。ELF 中的符号用于定位错误，
不代表启用了固件 debug 跟踪。发布不包括本地配置、录音和测试日志。

编译器来自 MounRiver 1.92 社区镜像，下载地址与 SHA-256 固定在
`references/release-toolchain.json`。缓存命中仍校验归档哈希。该编译器已在本地实际编译，
当前0.6.14源码产出的BIN与原本机工具链相同。WCH库哈希检查仍执行。

默认创建草稿；在 Releases 页面可编辑说明后发布。无需额外 PAT，使用仓库自带的
`GITHUB_TOKEN`，仅发布任务有 `contents: write` 权限。如果组织策略禁用了写权限，
需要管理员允许工作流发布。

标签创建是原子的；若创建标签后上传/发布失败，已有标签不会被重跑覆盖。检查该次
Actions 附件及标签指向后，手动完成该 Release，或换用新版本重跑。
自动构建不等于实机验收；已知硬件问题请随发布说明保留。
