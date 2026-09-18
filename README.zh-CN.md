# RK3588 EDID 设置工具

## 一、交叉编译

```shell
./build.sh
```

默认使用 RK3588 SDK 自带的 GCC 10.3 工具链，以及项目指定的 Debian 11
rootfs 作为 sysroot。输出文件为 `set_edid`。

在已安装 GCC 和 V4L2 头文件的 AArch64 设备上，也可以直接编译：

```shell
gcc -std=c11 -O2 -Wall -Wextra -Werror -o set_edid s_edid.c
```

## 二、根据分辨率自动生成 EDID

使用 `宽x高@帧率` 指定视频模式。常用电视模式使用标准 CTA-861 时序和 VIC，
其他电脑分辨率使用 CVT Reduced Blanking 时序：

```shell
./set_edid -r 1920x1080@60
./set_edid -r 1920x1080@30
./set_edid -r 1280x720@50
```

省略 `@帧率` 时默认使用 60 fps：

```shell
./set_edid -r 1920x1080
```

指定视频设备：

```shell
./set_edid -d /dev/video1 -r 1280x720@60
```

只生成并打印 EDID 数组，不访问设备：

```shell
./set_edid -r 2560x1440@30 -n
```

CVT 以 8 像素为水平粒度，因此 `1366x768` 会生成 `1368x768` 时序，并在
程序输出中明确显示请求值和实际值。

非 UHD 模式也沿用板卡原生 `RK-UHD` EDID 的身份、版本、显示参数及色度信息，
而不是使用合成身份。在 `192.168.20.19` 测试板上，这修复了 640x480@60、
2560x1080@60 和 2560x1440@60/75/120 的回退或无锁问题。

### 高分辨率与 HDMI 2.0

程序会自动判断是否需要 CTA 扩展块：

- 标准 `3840x2160@24/25/30/50/60` 使用板卡原生 `RK-UHD` EDID 作为
  兼容模板，并且只保留目标 VIC 93–97，避免信号源自动选择更高的 4K 帧率。
  原生身份、音频、物理地址和 HDMI VSDB 保持不变；基础块使用目标 3840 模式
  DTD，解决部分信号源忽略低帧率 CTA VIC 的问题。
- `3840x2160@24/25` 的标准水平前肩超过 EDID DTD 的 10 位字段上限，因此
  DTD 保持像素时钟和总时序不变，把前肩调整为 1020 像素并补偿到后肩。
- 标准 `4096x2160@24/25/30/50/60` 使用相同兼容模板和 VIC 98–102。
  4096 活动宽度无法写入 EDID 1.x 的 12 位 DTD。`@24` 同时宣告 HDMI
  VIC 4，在当前信号源上可锁定目标模式；`@25/30` 仍只靠 CTA VIC，当前
  信号源会回退到 1080p。
- `3840x2160@50/60` 按原生 EDID 通过 YCbCr 4:2:0 Video Data Block
  宣告，沿用 340 MHz TMDS 兼容链路，不再强制 594 MHz RGB/4:4:4 和 SCDC。
- `4096x2160@50/60` 同样只宣告目标 YCbCr 4:2:0 VIC。
- 分辨率达到 2560x1440，或像素时钟超过 340 MHz 时，自动生成第二个
  CTA 扩展块。
- `2560x1440` 等没有 CTA VIC 的自定义模式会同时写入基础块和 CTA DTD，
  并把该 DTD 标为原生时序；1080p/720p 只作为非原生回退模式。
- 自定义 CVT-RB 时序超过 600 MHz 时，会尝试 80 像素的紧凑水平消隐，
  仅在降低后不超过 600 MHz 才生成。`2560x1440@144` 在测试板上由约
  604 MHz 降至约 586 MHz 并成功锁定；仍超预算的模式继续报生成失败。
- 非标准高分辨率的扩展块包含 HDMI VSDB、HDMI Forum VSDB、基础音频和
  回退视频模式声明，但不再携带会触发 4K30 回退的旧 HDMI VIC 列表；标准
  4K 模式使用单目标 VIC 的 `RK-UHD` 兼容扩展块，DCI 4K24 的 HDMI VIC 4
  是经过单独板端验证的例外。
- 有标准 CTA VIC 的模式和高分辨率模式生成 256 字节 EDID；普通自定义电脑模式
  保持单块 128 字节 EDID。

例如：

```shell
./set_edid -r 3840x2160@30
./set_edid -r 3840x2160@60
./set_edid -r 4096x2160@60
```

对应的标准像素时钟分别为 297 MHz/VIC 95 和 594 MHz/VIC 97；其中
4K60 按原生模板限制为 YCbCr 4:2:0，实际 TMDS 字符率为 297 MHz。

## 三、其他设置方式

从文本文件加载 EDID，默认设置到 `/dev/video0`：

```shell
./set_edid -f edid.txt
```

使用程序内置的 EDID：

```shell
./set_edid -s 1920x1080_no_nv24
```

## 四、板端复测

`test_on_device.py` 会备份当前 EDID，逐项生成、写入、读回并查询两次输入
时序，最后恢复原 EDID；结果保存在带时间戳的 `test-results-*` 目录。
执行前确保 HDMI 信号源已连接，测试期间不要同时运行其他 EDID 工具：

```shell
python3 test_on_device.py
# 只测指定模式：
python3 test_on_device.py 2560x1440@60 4096x2160@24
```

程序显示 `set EDID success` 仅代表写入成功。应结合读回、实际输入时序和
HDMI RX 日志判定；测试脚本中的 `PASS` 代表锁定目标输入时序，未验证应用层
采集或显示画面。若脚本被强制杀死，需用测试目录中的 `original-edid.bin`
手动恢复。
