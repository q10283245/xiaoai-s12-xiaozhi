# 本地构建

所有命令只操作电脑上的文件。它们不连接音箱，不打开麦克风，不改启动项，不刷写分区。

## 环境

- Python 3.10+、Go 1.22+。
- Zig **0.13.0**；当前旧 glibc/ARM ABI 验收以这个版本为准。
- 运行 C 模拟测试需要 clang、Opus 和 json-c 开发库。Linux 可用 `pkg-config`；macOS 支持 Homebrew 路径发现。
- 用户自备的 S12A **1.76.54** page-aligned Android v0 `boot.img`，以及该版本的只读系统库。

不要把 OTA 容器直接当成 `boot.img`。输入必须有 `ANDROID!` 头、正确的组件校验和页对齐；构建器会核对已支持版本的镜像摘要。当前不支持猜测或自动适配其它原厂版本。

`--sysroot` 指向你自行提取的根目录，至少需要：

```text
usr/lib/libasound.so.2.0.0
usr/lib/libopus.so.0.6.1
usr/lib/libjson-c.so.2.0.2
lib/libc-2.25.so
lib/libpthread-2.25.so
lib/libm-2.25.so
lib/libdl-2.25.so
```

仓库不下载或携带这些厂商二进制。构建只用它们检查链接 ABI；运行时从音箱只读原厂 `system0` 加载库、loader、驱动和唤醒组件。不要把 sysroot 或原厂固件放入 Git。

## 1. 离线测试

```sh
make test
```

包含模拟 WebSocket/TLS、MCP、Opus、六麦前端、面板控制、提示音、配置、镜像重组和发布检查。测试中的本机临时服务器只收合成数据；不会访问某个真实云服务。启用了 ASan/UBSan 的 C 测试需要支持这些工具的 clang 环境。

## 2. 准备个人配置

```sh
mkdir -p local
python3 scripts/prepare_config.py \
  --output-dir local/device-config \
  --server-url wss://YOUR_SERVER/xiaozhi/v1/ \
  --device-id YOUR_SPEAKER_MAC \
  --ca-file YOUR_PUBLIC_CA_BUNDLE
```

将占位符换成实际值。SSID 交互输入，Wi-Fi 密码和上游小智令牌用隐藏输入。没有上游令牌时可留空；这不免除所选服务自身的设备绑定/激活要求。本机鉴权令牌单独随机生成。

CA 输入是你信任的**公共证书集合**，不是客户端私钥。根系统较精简，配置工具要求 WSS 提供 CA bundle；不会以关闭证书校验代替信任配置。内网明文 WS 只能使用显式的内网或回环 IP。

新配置目录权限为 `0700`，文件 `0600`。`client.json` 使用设备本机地址，`server.json` 指定真正的小智服务；令牌是文件引用。不要直接提交生成后的 JSON、Wi-Fi 配置、令牌或镜像。

## 3. 编译

```sh
python3 scripts/build.py \
  --zig YOUR_ZIG_0_13_EXECUTABLE \
  --sysroot YOUR_EXTRACTED_ROOTFS \
  --output-dir build
```

输出 `client.arm`、`panel.arm`、`bridge.arm` 与构建报告。客户端和面板是 ARM32 hard-float PIE，检查原厂 glibc 符号、`__fxstat64` ABI 与 COPY relocation；桥是静态 Linux ARM7 Go 程序。

构建器没有自动安装工具链的步骤。也可以用 `S12_ZIG`、`S12_SYSROOT` 指定输入；Go 工具可用 `--go` 指定。已有桥产物可通过 `--bridge` 指定，仍会检查其 ARM ELF 格式。

## 4. 生成个人试验镜像

```sh
python3 scripts/build_image.py \
  --stock-boot YOUR_ORIGINAL_BOOT_IMG \
  --build-dir build \
  --config-dir local/device-config \
  --output-dir local/candidate
```

输出目录必须不存在。构建器保留输入内核、DTB、无关头字段及原 CPIO 非 init 成员，只替换 init 并加入源码构建的程序和私有配置，重新计算 Android ID，限制镜像不超过 **8 MiB**。输出报告明确标记 `flash_written: false`、`hardware_tested: false`。

该镜像包含原厂组件和个人配置，**不是可公开分发的固件文件**。本仓库的开源交付物是源码；生成的 boot image 留在你的私有目录。

要固定镜像时间戳，可以设置 `SOURCE_DATE_EPOCH` 或 `--build-unix`。密码、随机本机令牌和 client UUID 不同时，镜像也会不同。

## 启动行为

候选只读挂载原厂 `system0`，由源码 init 启动 WPA/DHCP、面板、客户端及本机桥。提示音由数学波形生成；内嵌提示音可以用 `python3 scripts/generate_prompts.py --check` 校验再现性。

本地唤醒不等待联网。系统以构建时间作为时钟下限；桥的 `--sync-time` 只允许在音箱 PID1 的私有运行环境内启用，后台做有超时、带随机请求匹配的 SNTP，同步失败会重试，HTTPS 证书验证始终保留。SNTP 是尽力而为的非认证时间来源，不代表已获得可信时间服务。

首版镜像不带私有云侧车、注册数据库、发布令牌或环境写入器，也没有验证后自动写入 NAND 的逻辑。运行镜像与正式安装是不同步骤，请继续看 [安装与恢复](RECOVERY.md)。
