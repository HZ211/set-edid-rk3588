# RK3588 EDID 测试指南

本文档用于在 SmartLink RK3588 测试板上验证 `set_edid -r` 生成的 EDID 是否能
让 HDMI 信号源输出指定分辨率和帧率。

## 1. 测试环境

| 项目 | 地址或路径 |
| --- | --- |
| 开发/编译服务器 | `totainfo@192.168.112.130` |
| 项目目录 | `/home/totainfo/work/3.edid` |
| RK3588 测试板 | `pi@192.168.20.19` |
| 源码和编译程序 | `/home/pi/set-edid-rk3588` |
| 兼容入口 | `/home/pi/set_edid`（指向项目目录内的程序） |
| HDMI RX 设备 | `/dev/video0` |
| 常见模式清单 | `/home/pi/set-edid-rk3588/common_resolutions.txt` |

测试板使用 SSH 密码认证。密码不要写入 Git 仓库或测试报告，登录时在 SSH
提示符中输入已单独提供的测试板密码。

### 1.1 能直接访问测试板时

```bash
ssh pi@192.168.20.19
```

### 1.2 需要经过开发服务器时

分两步登录：

```bash
ssh totainfo@192.168.112.130
ssh pi@192.168.20.19
```

也可以在支持 ProxyJump 的 OpenSSH 客户端中使用：

```bash
ssh -J totainfo@192.168.112.130 pi@192.168.20.19
```

## 2. 测试前检查

确认程序存在并且可以执行：

```bash
ls -l /home/pi/set_edid
chmod +x /home/pi/set_edid
/home/pi/set_edid -h
```

确认 HDMI 信号源已连接且存在 5 V：

```bash
v4l2-ctl -d /dev/video0 --all | grep -A2 power_present
```

正常连接时应看到：

```text
power_present ... value=0x00000001
```

查看当前 EDID 和输入时序：

```bash
v4l2-ctl -d /dev/video0 --get-edid
v4l2-ctl -d /dev/video0 --query-dv-timings
```

## 3. 单个分辨率测试流程

下面以 `3840x2160@60` 为例。

### 3.1 仅生成 EDID，不写入设备

```bash
./set_edid -r 3840x2160@60 -n
```

检查以下内容：

- 程序返回值为 0；
- 输出的请求值、实际值和 Modeline 正确；
- EDID 为 128 或 256 字节；
- 高分辨率模式包含所需的 CTA 扩展块；
- 4K 标准模式显示正确的 CTA VIC。

查看返回值：

```bash
echo $?
```

### 3.2 写入 EDID

```bash
./set_edid -r 3840x2160@60
```

程序出现下面的输出只代表 V4L2 EDID 写入调用成功：

```text
set EDID success: /dev/video0
```

它不代表 HDMI RX 已经锁定目标信号，因此必须继续执行后面的读回、时序和日志
检查。

### 3.3 等待信号源重新读取 EDID

驱动写 EDID 时会触发 HPD 重新协商。一般等待 10～15 秒：

```bash
sleep 15
```

如果信号源缓存 EDID，需要拔插 HDMI、切换一次输出端口，或者重启信号源。

### 3.4 读回 EDID

```bash
v4l2-ctl -d /dev/video0 --get-edid
```

读回内容应与程序生成的数组一致。特别检查：

- 基础块第 126 字节的扩展块数量；
- 每个 128 字节块的校验和；
- CTA Video Data Block；
- 目标 CTA VIC 或自定义 DTD；
- 4K50/60 的 YCbCr 4:2:0 声明。

### 3.5 查询实际输入时序

```bash
v4l2-ctl -d /dev/video0 --query-dv-timings
v4l2-ctl -d /dev/video0 --get-dv-timings
```

成功时重点记录：

```text
Active width
Active height
Total width
Total height
Pixelclock
frames per second
Horizontal frontporch/sync/backporch
Vertical frontporch/sync/backporch
```

### 3.6 读取 HDMI RX 日志

```bash
dmesg | grep -Ei 'hdmirx|tmds|scdc|signal|format_change|avi_pkt' | tail -100
```

成功日志通常包含：

```text
signal lock ok
hdmirx_format_change: New format: 3840x2160p59.99 (4400x2250)
```

失败日志通常包含：

```text
signal not lock
tmds_clk_ratio:0
VIDIOC_QUERY_DV_TIMINGS: No locks available
```

## 4. 测试结果判定

| 结果 | 判定条件 |
| --- | --- |
| `PASS` | EDID 写入和读回正确，HDMI RX 锁定，宽高与生成的实际模式一致，帧率误差在合理范围内 |
| `FALLBACK` | HDMI RX 能锁定，但信号源输出了 1080p、720p 等其他分辨率 |
| `NO_LOCK` | EDID 写入成功，但等待后 `query-dv-timings` 仍报告 `No locks available` |
| `EDID_MISMATCH` | `/dev/video0` 读回的 EDID 与本次生成内容不一致 |
| `GENERATE_FAIL` | `set_edid -r` 无法生成该模式或返回非 0 |
| `UNSTABLE` | 能短暂锁定，但反复出现掉锁、格式切换或 AVI InfoFrame 接收失败 |

建议在首次锁定后再等待 5 秒并重复查询一次。两次宽高、总时序和帧率一致，才记录
为稳定的 `PASS`。

## 5. 批量测试方法

每个模式都必须留出重新协商时间。下面是小批量测试示例：

```bash
modes=(
    640x480@60
    800x600@60
    1024x768@60
    1280x720@60
    1920x1080@60
    2560x1440@60
    3840x2160@30
    3840x2160@60
)

for mode in "${modes[@]}"; do
    echo "===== $mode ====="
    ./set_edid -r "$mode" > "/tmp/set_edid-${mode}.log" 2>&1
    rc=$?
    echo "set_edid_rc=$rc"
    if [ "$rc" -ne 0 ]; then
        echo "result=GENERATE_FAIL"
        continue
    fi

    sleep 15
    v4l2-ctl -d /dev/video0 --query-dv-timings 2>&1 |
        tee "/tmp/timing-${mode}.log"
    sleep 5
    v4l2-ctl -d /dev/video0 --query-dv-timings 2>&1 |
        tee -a "/tmp/timing-${mode}.log"
done
```

注意：这个循环只负责设置和保存原始结果，最终仍应按照第 4 节核对实际宽高和
帧率，不能仅根据 `set EDID success` 判断通过。

完整待测分辨率和推荐帧率见：

```bash
less /home/pi/set-edid-rk3588/common_resolutions.txt
```

## 6. 建议的测试顺序

为减少链路带宽变化带来的干扰，建议从低到高测试：

1. `640x480@60`
2. `800x600@60`
3. `1024x768@60`
4. `1280x720@60`
5. `1920x1080@60`
6. `1920x1200@60`
7. `2560x1440@60`
8. `3840x2160@30`
9. `3840x2160@60`

同一分辨率需要测试多个帧率时，也建议按帧率从低到高执行。

## 7. 恢复板卡原生 EDID

测试结束后执行：

```bash
./set_edid -m
sleep 15
v4l2-ctl -d /dev/video0 --get-edid
v4l2-ctl -d /dev/video0 --query-dv-timings
```

`-m` 使用项目中的原生 `RK-UHD` 256 字节 EDID。

## 8. 当前已验证结果（2026-09-18，192.168.20.19）

测试使用设备端编译的程序，逐项等待 12 秒，再间隔 3 秒查询两次输入时序；
25 个模式中 20 个 `PASS`、2 个 `FALLBACK`、3 个 `GENERATE_FAIL`。
所有成功生成并写入的模式，EDID 读回均与生成数组一致；结束后原 EDID
逐字节恢复。详细原始记录在设备项目目录的 `test-results-20260918-141305`。

| 模式 | 结果 | 实际输入或原因 |
| --- | --- | --- |
| `640x480@60` | `PASS` | 640x480@59.99 |
| `1920x1080@60` | `PASS` | 1920x1080@59.99 |
| `2560x1080@60/75` | `PASS` | 约 59.97 / 74.90 fps |
| `2560x1440@30/60/75/120/144` | `PASS` | 约 29.94 / 59.95 / 74.96 / 119.93 / 143.91 fps |
| `3440x1440@60/75/100` | `PASS` | 约 59.97 / 74.93 / 99.93 fps |
| `3840x2160@24/25/30/50/60` | `PASS` | 目标帧率均正确锁定 |
| `4096x2160@24/50/60` | `PASS` | 约 24.00 / 50.00 / 59.93 fps |
| `4096x2160@25/30` | `FALLBACK` | 回退 1920x1080@约 60 fps |
| `2560x1440@165` | `GENERATE_FAIL` | 当前链路时钟预算不满足 |
| `3440x1440@120/144` | `GENERATE_FAIL` | 当前链路时钟预算不满足 |

与 2026-08-28 的旧报告相比，复用原生 EDID 身份字段解决了多项低分辨率和
QHD 回退/无锁；紧凑消隐解决 QHD144 的 604 MHz 超链路预算问题；HDMI VIC 4
使 DCI 4K24 正常锁定。`4096x2160@25/30` 仍未解决：这两个模式无法写入
EDID 1.x 的基础 DTD，当前信号源没有按 CTA VIC 99/100 输出目标时序。
不要把此结论外推到其他信号源或板卡。
