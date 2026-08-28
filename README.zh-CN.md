# RK3588 EDID 设置工具

## 一、交叉编译

```shell
./build.sh
```

默认使用 RK3588 SDK 自带的 GCC 10.3 工具链，以及项目指定的 Debian 11
rootfs 作为 sysroot。输出文件为 `set_edid`。

## 二、根据分辨率自动生成 EDID

使用 `宽x高@帧率` 指定视频模式，程序会生成对应的 CVT Reduced Blanking
时序：

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

只生成并打印 128 字节 EDID 数组，不访问设备：

```shell
./set_edid -r 2560x1440@30 -n
```

CVT 以 8 像素为水平粒度，因此 `1366x768` 会生成 `1368x768` 时序，并在
程序输出中明确显示请求值和实际值。

### 高分辨率与 HDMI 2.0

程序会自动判断是否需要 CTA 扩展块：

- 标准 `3840x2160@24/25/30/50/60` 使用板卡原生 `RK-UHD` EDID 作为
  兼容模板，并把目标 VIC 93–97 标记为原生模式。基础块、音频、物理地址、
  HDMI VSDB 和兼容 DTD 均保持原生布局。
- `3840x2160@50/60` 按原生 EDID 通过 YCbCr 4:2:0 Video Data Block
  宣告，沿用 340 MHz TMDS 兼容链路，不再强制 594 MHz RGB/4:4:4 和 SCDC。
- 分辨率达到 2560x1440，或像素时钟超过 340 MHz 时，自动生成第二个
  CTA 扩展块。
- `2560x1440` 等没有 CTA VIC 的自定义模式会同时写入基础块和 CTA DTD，
  并把该 DTD 标为原生时序；1080p/720p 只作为非原生回退模式。
- 非标准高分辨率的扩展块包含 HDMI VSDB、HDMI Forum VSDB、基础音频和
  回退视频模式声明；标准 4K 模式则保留原生 `RK-UHD` 扩展块内容。
- 普通模式保持单块 128 字节 EDID；高分辨率模式自动生成 256 字节 EDID。

例如：

```shell
./set_edid -r 3840x2160@30
./set_edid -r 3840x2160@60
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
