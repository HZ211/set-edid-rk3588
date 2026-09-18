#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <getopt.h>
#include <errno.h>
#include <ctype.h>
#include <limits.h>
#include <stdint.h>

#include "set_edid.h"

struct edid_map edid_table[] = {
    {"800x600", edid_800x600_data, 1},
    {"1024x768", edid_1024x768_data, 1},
    {"1280x720", edid_1280x720_data, 1},
    {"1280x800", edid_1280x800_data, 1},
    {"1280x1024", edid_1280x1024_data, 1},
    {"1366x768", edid_1366x768_data, 1},
    {"1440x900", edid_1440x900_data, 1},
    {"1600x900", edid_1600x900_data, 1},
    {"1600x1050", edid_1600x1050_data, 1},
    {"1920x1200", edid_1920x1200_data, 1},
    {"1920x1080_no_nv24", edid_1920x1080_no_nv24_data, 2},
};

#define PER_BLOCK 128
#define MAX_EDID_BLOCKS 2
#define MAX_EDID_SIZE (PER_BLOCK * MAX_EDID_BLOCKS)
#define DEFAULT_REFRESH_HZ 60
#define CVT_CLOCK_STEP_KHZ 250
#define CVT_RB_MIN_VBLANK_US 460
#define CVT_RB_H_SYNC 32
#define CVT_RB_H_BLANK 160
#define CVT_COMPACT_H_BLANK 80
#define CVT_COMPACT_H_FRONT_PORCH 8
#define HDMI_MAX_TMDS_CLOCK_KHZ 600000
#define CVT_RB_V_FRONT_PORCH 3
#define CVT_MIN_V_BACK_PORCH 6

struct video_timing {
    unsigned int hdisplay;
    unsigned int hsync_start;
    unsigned int hsync_end;
    unsigned int htotal;
    unsigned int vdisplay;
    unsigned int vsync_start;
    unsigned int vsync_end;
    unsigned int vtotal;
    unsigned int pixel_clock_khz;
    unsigned int cea_vic;
    unsigned char dtd_flags;
};

struct cea_mode {
    unsigned int width;
    unsigned int height;
    unsigned int frame_rate;
    unsigned int pixel_clock_khz;
    unsigned int hfront;
    unsigned int hsync;
    unsigned int hback;
    unsigned int vfront;
    unsigned int vsync;
    unsigned int vback;
    unsigned int vic;
    unsigned char dtd_flags;
};

static void print_usage(const char *program_name)
{
    const char *name = program_name ? program_name : "set_edid";

    printf("用法: %s [选项]\n", name);
    printf("选项:\n");
    printf("  -d <设备>       指定视频设备节点 (默认: /dev/video0)\n");
    printf("  -f <文件>       从文件加载 EDID 数据\n");
    printf("  -s <分辨率>     使用内置 EDID，例如 1920x1080_no_nv24\n");
    printf("  -r <宽x高@帧率> 自动生成 EDID 并设置 (默认 60 fps)\n");
    printf("  -n              仅打印由 -r 生成的 EDID，不设置设备\n");
    printf("  -p              打印设备当前 EDID\n");
    printf("  -m              设置默认 EDID\n");
    printf("  -h              显示此帮助信息\n");
    printf("\n示例:\n");
    printf("  %s -r 1920x1080@60              # 生成 1080p 60fps EDID\n", name);
    printf("  %s -r 1920x1080@30 -n           # 仅生成 1080p 30fps 数组\n", name);
    printf("  %s -d /dev/video1 -r 1280x720@50 # 设置到指定设备\n", name);
    printf("  %s -s 1920x1080_no_nv24         # 使用已有 EDID\n", name);
    printf("  %s -f edid.txt                   # 从文件加载 EDID\n", name);
    printf("\n标准 HDMI 模式自动使用 CTA-861 时序和对应 VIC。\n");

    printf("\n当前内置分辨率:\n");
    for (size_t i = 0; i < sizeof(edid_table) / sizeof(edid_table[0]); ++i)
        printf("  %s\n", edid_table[i].fmt);
}

static int parse_video_mode(const char *text, unsigned int *width,
                            unsigned int *height, unsigned int *frame_rate)
{
    char *separator;
    char *end;
    char *frame_rate_end;
    unsigned long parsed_width;
    unsigned long parsed_height;
    unsigned long parsed_frame_rate = DEFAULT_REFRESH_HZ;

    if (!text || !width || !height || !frame_rate)
        return -1;

    errno = 0;
    parsed_width = strtoul(text, &separator, 10);
    if (errno || separator == text || (*separator != 'x' && *separator != 'X'))
        return -1;

    errno = 0;
    parsed_height = strtoul(separator + 1, &end, 10);
    if (errno || end == separator + 1)
        return -1;

    if (*end == '@') {
        errno = 0;
        parsed_frame_rate = strtoul(end + 1, &frame_rate_end, 10);
        if (errno || frame_rate_end == end + 1 || *frame_rate_end != '\0')
            return -1;
    } else if (*end != '\0') {
        return -1;
    }

    if (parsed_width < 320 || parsed_width > 4096 ||
        parsed_height < 200 || parsed_height > 2160 ||
        parsed_frame_rate < 1 || parsed_frame_rate > 240)
        return -1;

    *width = (unsigned int)parsed_width;
    *height = (unsigned int)parsed_height;
    *frame_rate = (unsigned int)parsed_frame_rate;
    return 0;
}

static unsigned int cvt_vsync_width(unsigned int width, unsigned int height)
{
    if (height % 3 == 0 && height * 4 / 3 == width)
        return 4;
    if (height % 9 == 0 && height * 16 / 9 == width)
        return 5;
    if (height % 10 == 0 && height * 16 / 10 == width)
        return 6;
    if (height % 4 == 0 && height * 5 / 4 == width)
        return 7;
    if (height % 9 == 0 && height * 15 / 9 == width)
        return 7;
    return 10;
}

static int generate_cvt_rb_timing(unsigned int requested_width,
                                  unsigned int height,
                                  unsigned int refresh_hz,
                                  struct video_timing *timing)
{
    uint64_t hperiod_ns;
    uint64_t pixel_clock_khz;
    unsigned int width;
    unsigned int vsync_width;
    unsigned int vblank_lines;
    unsigned int minimum_vblank;

    if (!timing || !refresh_hz)
        return -1;

    /* CVT uses an 8-pixel character cell. Match the cvt utility by rounding
     * to the nearest cell (for example, 1366 becomes 1368).
     */
    width = ((requested_width + 4) / 8) * 8;
    if (width > 4095)
        return -1;

    vsync_width = cvt_vsync_width(width, height);
    hperiod_ns = 1000000000ULL -
                 (uint64_t)CVT_RB_MIN_VBLANK_US * 1000 * refresh_hz;
    hperiod_ns /= (uint64_t)height * refresh_hz;
    if (!hperiod_ns)
        return -1;

    vblank_lines = CVT_RB_MIN_VBLANK_US * 1000ULL / hperiod_ns + 1;
    minimum_vblank = CVT_RB_V_FRONT_PORCH + vsync_width +
                     CVT_MIN_V_BACK_PORCH;
    if (vblank_lines < minimum_vblank)
        vblank_lines = minimum_vblank;

    timing->hdisplay = width;
    timing->hsync_start = width + CVT_RB_H_BLANK / 2 - CVT_RB_H_SYNC;
    timing->hsync_end = width + CVT_RB_H_BLANK / 2;
    timing->htotal = width + CVT_RB_H_BLANK;
    timing->vdisplay = height;
    timing->vsync_start = height + CVT_RB_V_FRONT_PORCH;
    timing->vsync_end = timing->vsync_start + vsync_width;
    timing->vtotal = height + vblank_lines;
    timing->cea_vic = 0;
    timing->dtd_flags = 0x1a; /* Positive HSync, negative VSync. */

    pixel_clock_khz = (uint64_t)timing->htotal * 1000000ULL / hperiod_ns;
    pixel_clock_khz -= pixel_clock_khz % CVT_CLOCK_STEP_KHZ;
    if (pixel_clock_khz > HDMI_MAX_TMDS_CLOCK_KHZ) {
        uint64_t compact_clock_khz;

        /* A shorter horizontal blank can keep high-refresh custom modes
         * inside the 600 MHz HDMI link budget. QHD@144 was verified on the
         * RK3588 receiver with 8/32/40-pixel front/sync/back porches.
         */
        compact_clock_khz =
            (uint64_t)(width + CVT_COMPACT_H_BLANK) * 1000000ULL /
            hperiod_ns;
        compact_clock_khz -= compact_clock_khz % CVT_CLOCK_STEP_KHZ;
        if (compact_clock_khz <= HDMI_MAX_TMDS_CLOCK_KHZ) {
            timing->hsync_start = width + CVT_COMPACT_H_FRONT_PORCH;
            timing->hsync_end = timing->hsync_start + CVT_RB_H_SYNC;
            timing->htotal = width + CVT_COMPACT_H_BLANK;
            pixel_clock_khz = compact_clock_khz;
        }
    }
    if (!pixel_clock_khz || pixel_clock_khz > HDMI_MAX_TMDS_CLOCK_KHZ ||
        timing->htotal > 4095 || timing->vtotal > 4095)
        return -1;

    timing->pixel_clock_khz = (unsigned int)pixel_clock_khz;
    return 0;
}

static int generate_cea_timing(unsigned int width, unsigned int height,
                               unsigned int frame_rate,
                               struct video_timing *timing)
{
    static const struct cea_mode modes[] = {
        { 640,  480, 60,  25175,   16, 96,  48, 10,  2, 33,   1, 0x18},
        {1280,  720, 50,  74250,  440, 40, 220,  5,  5, 20,  19, 0x1e},
        {1280,  720, 60,  74250,  110, 40, 220,  5,  5, 20,   4, 0x1e},
        {1920, 1080, 24,  74250,  638, 44, 148,  4,  5, 36,  32, 0x1e},
        {1920, 1080, 25,  74250,  528, 44, 148,  4,  5, 36,  33, 0x1e},
        {1920, 1080, 30,  74250,   88, 44, 148,  4,  5, 36,  34, 0x1e},
        {1920, 1080, 50, 148500,  528, 44, 148,  4,  5, 36,  31, 0x1e},
        {1920, 1080, 60, 148500,   88, 44, 148,  4,  5, 36,  16, 0x1e},
        {3840, 2160, 24, 297000, 1276, 88, 296,  8, 10, 72,  93, 0x1e},
        {3840, 2160, 25, 297000, 1056, 88, 296,  8, 10, 72,  94, 0x1e},
        {3840, 2160, 30, 297000,  176, 88, 296,  8, 10, 72,  95, 0x1e},
        {3840, 2160, 50, 594000, 1056, 88, 296,  8, 10, 72,  96, 0x1e},
        {3840, 2160, 60, 594000,  176, 88, 296,  8, 10, 72,  97, 0x1e},
        {4096, 2160, 24, 297000, 1020, 88, 296,  8, 10, 72,  98, 0x1e},
        {4096, 2160, 25, 297000,  968, 88, 128,  8, 10, 72,  99, 0x1e},
        {4096, 2160, 30, 297000,   88, 88, 128,  8, 10, 72, 100, 0x1e},
        {4096, 2160, 50, 594000,  968, 88, 128,  8, 10, 72, 101, 0x1e},
        {4096, 2160, 60, 594000,   88, 88, 128,  8, 10, 72, 102, 0x1e},
    };

    if (!timing)
        return -1;

    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); ++i) {
        const struct cea_mode *mode = &modes[i];

        if (mode->width != width || mode->height != height ||
            mode->frame_rate != frame_rate)
            continue;

        timing->hdisplay = mode->width;
        timing->hsync_start = mode->width + mode->hfront;
        timing->hsync_end = timing->hsync_start + mode->hsync;
        timing->htotal = timing->hsync_end + mode->hback;
        timing->vdisplay = mode->height;
        timing->vsync_start = mode->height + mode->vfront;
        timing->vsync_end = timing->vsync_start + mode->vsync;
        timing->vtotal = timing->vsync_end + mode->vback;
        timing->pixel_clock_khz = mode->pixel_clock_khz;
        timing->cea_vic = mode->vic;
        timing->dtd_flags = mode->dtd_flags;
        return 0;
    }

    return -1;
}

static void generate_1080p60_fallback(struct video_timing *timing)
{
    timing->hdisplay = 1920;
    timing->hsync_start = 2008;
    timing->hsync_end = 2052;
    timing->htotal = 2200;
    timing->vdisplay = 1080;
    timing->vsync_start = 1084;
    timing->vsync_end = 1089;
    timing->vtotal = 1125;
    timing->pixel_clock_khz = 148500;
    timing->cea_vic = 16;
    timing->dtd_flags = 0x1e;
}

static int write_detailed_timing(unsigned char *descriptor,
                                 const struct video_timing *timing)
{
    unsigned int pixel_clock_10khz = timing->pixel_clock_khz / 10;
    unsigned int hblank = timing->htotal - timing->hdisplay;
    unsigned int vblank = timing->vtotal - timing->vdisplay;
    unsigned int hsync_offset = timing->hsync_start - timing->hdisplay;
    unsigned int hsync_width = timing->hsync_end - timing->hsync_start;
    unsigned int vsync_offset = timing->vsync_start - timing->vdisplay;
    unsigned int vsync_width = timing->vsync_end - timing->vsync_start;

    if (!descriptor || pixel_clock_10khz > UINT16_MAX ||
        hblank > 4095 || vblank > 4095 ||
        hsync_offset > 1023 || hsync_width > 1023 ||
        vsync_offset > 63 || vsync_width > 63)
        return -1;

    memset(descriptor, 0, 18);
    descriptor[0] = pixel_clock_10khz & 0xff;
    descriptor[1] = pixel_clock_10khz >> 8;
    descriptor[2] = timing->hdisplay & 0xff;
    descriptor[3] = hblank & 0xff;
    descriptor[4] = ((timing->hdisplay >> 8) << 4) | (hblank >> 8);
    descriptor[5] = timing->vdisplay & 0xff;
    descriptor[6] = vblank & 0xff;
    descriptor[7] = ((timing->vdisplay >> 8) << 4) | (vblank >> 8);
    descriptor[8] = hsync_offset & 0xff;
    descriptor[9] = hsync_width & 0xff;
    descriptor[10] = ((vsync_offset & 0x0f) << 4) |
                     (vsync_width & 0x0f);
    descriptor[11] = ((hsync_offset >> 8) << 6) |
                     ((hsync_width >> 8) << 4) |
                     ((vsync_offset >> 4) << 2) |
                     (vsync_width >> 4);
    descriptor[17] = timing->dtd_flags;
    return 0;
}

static void write_text_descriptor(unsigned char *descriptor, unsigned char tag,
                                  const char *text)
{
    size_t length = strlen(text);

    if (length > 13)
        length = 13;

    memset(descriptor, 0, 18);
    descriptor[3] = tag;
    memset(descriptor + 5, ' ', 13);
    memcpy(descriptor + 5, text, length);
    if (length < 13)
        descriptor[5 + length] = '\n';
}

static unsigned char calculate_block_checksum(const unsigned char *block)
{
    unsigned int sum = 0;

    for (size_t i = 0; i < PER_BLOCK - 1; ++i)
        sum += block[i];
    return (unsigned char)(0x100 - (sum & 0xff));
}

static int generate_native_compatible_uhd_edid(
    unsigned int native_vic, const struct video_timing *timing,
    unsigned char edid[MAX_EDID_SIZE])
{
    static const unsigned char audio_block[] = {
        0x23, 0x09, 0x07, 0x07
    };
    static const unsigned char speaker_block[] = {
        0x83, 0x01, 0x00, 0x00
    };
    static const unsigned char hdmi_vsdb[] = {
        0x67, 0x03, 0x0c, 0x00, 0x30, 0x00, 0x00, 0x44
    };
    static const unsigned char hdmi_vsdb_dci24[] = {
        0x6b, 0x03, 0x0c, 0x00, 0x30, 0x00, 0x00, 0x44,
        0x20, 0x00, 0x20, 0x04 /* HDMI VIC 4: 4096x2160@24 */
    };
    static const unsigned char colorimetry_block[] = {
        0xe3, 0x05, 0x03, 0x01
    };
    unsigned char *extension;
    struct video_timing base_dtd;
    uint64_t pixels_per_frame;
    unsigned int vertical_hz;
    unsigned int horizontal_khz;
    unsigned int maximum_clock_10mhz;
    size_t position = 4;

    if (!edid || !timing || native_vic < 93 || native_vic > 102 ||
        timing->cea_vic != native_vic ||
        sizeof(default_edid_data) < MAX_EDID_SIZE)
        return -1;

    /* Keep the board's original identification and HDMI capability blocks,
     * but remove every base-block fallback timing. Some sources always select
     * the first base DTD even when the requested CTA VIC is marked native.
     * A single-mode EDID makes the requested UHD/DCI VIC unambiguous.
     */
    memcpy(edid, default_edid_data, MAX_EDID_SIZE);
    /* The fourth base descriptor is the template's range limits. Its
     * original 59-70 Hz range conflicts with requested 24/25/30/50 Hz
     * native VICs, so describe the timing actually advertised below.
     * Reject a changed template rather than overwriting another descriptor.
     */
    if (edid[108] != 0 || edid[109] != 0 || edid[110] != 0 ||
        edid[111] != 0xfd || edid[112] != 0 ||
        !timing->htotal || !timing->vtotal || !timing->pixel_clock_khz)
        return -1;
    pixels_per_frame = (uint64_t)timing->htotal * timing->vtotal;
    vertical_hz = (unsigned int)(
        ((uint64_t)timing->pixel_clock_khz * 1000ULL +
         pixels_per_frame / 2) / pixels_per_frame);
    horizontal_khz = (timing->pixel_clock_khz + timing->htotal - 1) /
                     timing->htotal;
    maximum_clock_10mhz = (timing->pixel_clock_khz + 9999) / 10000;
    if (!vertical_hz || !horizontal_khz ||
        vertical_hz > 250 || horizontal_khz > 250 ||
        maximum_clock_10mhz > 255)
        return -1;
    edid[113] = vertical_hz > 5 ? vertical_hz - 5 : 1;
    edid[114] = vertical_hz + 5;
    edid[115] = horizontal_khz > 5 ? horizontal_khz - 5 : 1;
    edid[116] = horizontal_khz + 5;
    edid[117] = maximum_clock_10mhz;
    edid[24] &= ~0x02; /* No preferred base-block timing. */
    memset(edid + 35, 0, 3); /* No established timings. */
    for (size_t i = 38; i < 54; ++i)
        edid[i] = 0x01; /* No standard timings. */
    memset(edid + 54, 0, 36); /* No base-block DTDs. */

    /* EDID 1.x cannot encode a 4096-pixel active width. For 3840 modes, add
     * a preferred DTD so sources that ignore low-refresh CTA VICs still select
     * the requested mode. Keep the CTA total and pixel clock; only move an
     * overlarge horizontal sync offset into the DTD's 10-bit range.
     */
    if (timing->hdisplay <= 4095) {
        unsigned int hsync_offset;

        base_dtd = *timing;
        hsync_offset = base_dtd.hsync_start - base_dtd.hdisplay;
        if (hsync_offset > 1020) {
            unsigned int adjustment = hsync_offset - 1020;

            base_dtd.hsync_start -= adjustment;
            base_dtd.hsync_end -= adjustment;
        }
        if (write_detailed_timing(edid + 54, &base_dtd) < 0)
            return -1;
        edid[24] |= 0x02;
    }
    extension = edid + PER_BLOCK;
    memset(extension, 0, PER_BLOCK);
    extension[0] = 0x02;
    extension[1] = 0x03;
    extension[3] = 0xf0; /* Original color/audio flags, no native CTA DTD. */

    extension[position++] = 0x41;
    extension[position++] = 0x80 | native_vic;

#define APPEND_NATIVE_BLOCK(data) do { \
        memcpy(extension + position, (data), sizeof(data)); \
        position += sizeof(data); \
    } while (0)
    APPEND_NATIVE_BLOCK(audio_block);
    APPEND_NATIVE_BLOCK(speaker_block);
    if (native_vic == 98) {
        APPEND_NATIVE_BLOCK(hdmi_vsdb_dci24);
    } else {
        APPEND_NATIVE_BLOCK(hdmi_vsdb);
    }
    APPEND_NATIVE_BLOCK(colorimetry_block);
#undef APPEND_NATIVE_BLOCK

    if (native_vic == 96 || native_vic == 97 ||
        native_vic == 101 || native_vic == 102) {
        extension[position++] = 0xe2;
        extension[position++] = 0x0e; /* YCbCr 4:2:0 Video Data Block */
        extension[position++] = (unsigned char)native_vic;
    }

    extension[2] = (unsigned char)position;

    edid[127] = calculate_block_checksum(edid);
    extension[127] = calculate_block_checksum(extension);
    for (unsigned int block = 0; block < MAX_EDID_BLOCKS; ++block) {
        unsigned int sum = 0;

        for (size_t i = 0; i < PER_BLOCK; ++i)
            sum += edid[block * PER_BLOCK + i];
        if ((sum & 0xff) != 0)
            return -1;
    }
    return 0;
}

static int generate_cta_extension(unsigned char extension[PER_BLOCK],
                                  unsigned int native_vic,
                                  const struct video_timing *native_dtd)
{
    static const unsigned char audio_block[] = {
        0x23, 0x09, 0x07, 0x07
    };
    static const unsigned char speaker_block[] = {
        0x83, 0x01, 0x00, 0x00
    };
    static const unsigned char hdmi_vsdb[] = {
        0x67, 0x03, 0x0c, 0x00, 0x30, 0x00, 0x00, 0x44
    };
    static const unsigned char hdmi_forum_vsdb[] = {
        0x67, 0xd8, 0x5d, 0xc4, 0x01, 0x78, 0xc0, 0x00
    };
    static const unsigned char colorimetry_block[] = {
        0xe3, 0x05, 0x03, 0x01
    };
    static const unsigned char video_capability_block[] = {
        0xe2, 0x00, 0xcb
    };
    unsigned char svds[3];
    unsigned int svd_count = 0;
    size_t position = 4;

    if (!extension || native_vic > 127 || (native_vic && native_dtd))
        return -1;

    memset(extension, 0, PER_BLOCK);
    extension[0] = 0x02; /* CTA extension tag */
    extension[1] = 0x03; /* CTA-861 revision 3 */
    extension[3] = 0xf0; /* Basic audio, YCbCr 4:4:4 and 4:2:2 */

    if (native_vic) {
        svds[svd_count++] = 0x80 | native_vic;
        if (native_vic == 4 || native_vic == 19) {
            svds[svd_count++] = 1;
        } else if (native_vic != 1) {
            svds[svd_count++] = 4;
            svds[svd_count++] = 1;
        }
    } else {
        /* Custom modes have no CTA VIC. Do not incorrectly mark VIC 16 as
         * native: the custom DTD below is the preferred/native timing.
         */
        svds[svd_count++] = 16;
        svds[svd_count++] = 4;
        svds[svd_count++] = 1;
    }
    extension[position++] = 0x40 | svd_count;
    memcpy(extension + position, svds, svd_count);
    position += svd_count;

#define APPEND_CTA_BLOCK(data) do { \
        memcpy(extension + position, (data), sizeof(data)); \
        position += sizeof(data); \
    } while (0)
    APPEND_CTA_BLOCK(audio_block);
    APPEND_CTA_BLOCK(speaker_block);
    APPEND_CTA_BLOCK(hdmi_vsdb);
    APPEND_CTA_BLOCK(hdmi_forum_vsdb);
    APPEND_CTA_BLOCK(colorimetry_block);
    APPEND_CTA_BLOCK(video_capability_block);
#undef APPEND_CTA_BLOCK

    if (position >= PER_BLOCK - 1)
        return -1;

    extension[2] = (unsigned char)position;
    if (native_dtd) {
        if (position + 18 > PER_BLOCK - 1 ||
            write_detailed_timing(extension + position, native_dtd) < 0)
            return -1;
        extension[3] |= 0x01; /* One native DTD follows the data blocks. */
    }
    extension[127] = calculate_block_checksum(extension);
    return 0;
}

static int generate_edid_array(unsigned int requested_width,
                               unsigned int requested_height,
                               unsigned int frame_rate,
                               unsigned char edid[MAX_EDID_SIZE],
                               struct video_timing *generated_timing,
                               unsigned int *generated_blocks)
{
    static const unsigned char header[8] = {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
    };
    struct video_timing timing;
    struct video_timing base_dtd_timing;
    char monitor_name[14];
    unsigned int hsync_khz;
    unsigned int maximum_clock_10mhz;
    unsigned int blocks = 1;
    int is_cea_timing;
    int needs_cta_extension;

    if (!edid)
        return -1;

    is_cea_timing = generate_cea_timing(requested_width, requested_height,
                                        frame_rate, &timing) == 0;
    if (!is_cea_timing &&
        generate_cvt_rb_timing(requested_width, requested_height,
                               frame_rate, &timing) < 0)
        return -1;

    if (timing.cea_vic >= 93 && timing.cea_vic <= 102) {
        if (generate_native_compatible_uhd_edid(timing.cea_vic, &timing,
                                                edid) < 0)
            return -1;
        if (generated_timing)
            *generated_timing = timing;
        if (generated_blocks)
            *generated_blocks = 2;
        return 0;
    }

    needs_cta_extension = timing.cea_vic != 0 ||
                          timing.hdisplay >= 2560 || timing.vdisplay >= 1440 ||
                          timing.pixel_clock_khz > 340000;

    memset(edid, 0, MAX_EDID_SIZE);
    memcpy(edid, header, sizeof(header));
    /* Keep the original RK-UHD identity, EDID revision, display parameters,
     * and chromaticity even for non-UHD modes. On the tested HDMI source,
     * synthetic values made otherwise valid 640x480 and QHD EDIDs fall back
     * or fail to lock. Timing descriptors below remain mode-specific.
     */
    memcpy(edid + 8, default_edid_data + 8, 27);
    for (size_t i = 38; i < 54; ++i)
        edid[i] = 0x01;  /* Unused standard timing slots */

    base_dtd_timing = timing;
    if (write_detailed_timing(edid + 54, &base_dtd_timing) < 0) {
        /* CTA VICs 93/94/96 have a sync offset too large for the 10-bit DTD
         * field. Advertise the requested mode as the native CTA SVD and keep
         * a widely compatible 1080p60 DTD in the base block.
         */
        if (!timing.cea_vic)
            return -1;
        generate_1080p60_fallback(&base_dtd_timing);
        if (write_detailed_timing(edid + 54, &base_dtd_timing) < 0)
            return -1;
        edid[24] &= ~0x02; /* Native CTA SVD, not base DTD, is preferred. */
    }

    snprintf(monitor_name, sizeof(monitor_name), "RK-%ux%u",
             timing.hdisplay, timing.vdisplay);
    write_text_descriptor(edid + 72, 0xfc, monitor_name);

    memset(edid + 90, 0, 18);
    edid[93] = 0xfd;     /* Monitor range limits descriptor */
    edid[95] = frame_rate > 5 ? frame_rate - 5 : 1;
    edid[96] = frame_rate < 250 ? frame_rate + 5 : 255;
    edid[97] = 20;       /* Minimum horizontal rate (kHz) */
    hsync_khz = (timing.pixel_clock_khz + timing.htotal - 1) / timing.htotal;
    edid[98] = hsync_khz < 250 ? hsync_khz + 5 : 255;
    maximum_clock_10mhz = (timing.pixel_clock_khz + 9999) / 10000;
    edid[99] = maximum_clock_10mhz < 255 ? maximum_clock_10mhz : 255;

    write_text_descriptor(edid + 108, 0xff,
                          is_cea_timing ? "CTA-MODE" : "AUTO-CVT-RB");

    if (needs_cta_extension) {
        const struct video_timing *cta_native_dtd =
            timing.cea_vic ? NULL : &timing;

        if (generate_cta_extension(edid + PER_BLOCK, timing.cea_vic,
                                   cta_native_dtd) < 0)
            return -1;
        blocks = 2;
    }

    edid[126] = blocks - 1;
    edid[127] = calculate_block_checksum(edid);

    for (unsigned int block = 0; block < blocks; ++block) {
        unsigned int checksum = 0;

        for (size_t i = 0; i < PER_BLOCK; ++i)
            checksum += edid[block * PER_BLOCK + i];
        if ((checksum & 0xff) != 0)
            return -1;
    }

    if (generated_timing)
        *generated_timing = timing;
    if (generated_blocks)
        *generated_blocks = blocks;
    return 0;
}

static void print_edid_array(const unsigned char *edid, unsigned int blocks)
{
    size_t size = (size_t)blocks * PER_BLOCK;

    printf("unsigned char generated_edid_data[%zu] = {\n", size);
    for (size_t i = 0; i < size; ++i) {
        if (i % 8 == 0)
            printf("    ");
        printf("0x%02X%s", edid[i], i + 1 == size ? "" : ", ");
        if (i % 8 == 7)
            printf("\n");
    }
    printf("};\n");
}

static unsigned char *parse_hex_string(const char *input, int *out_size)
{
    int count = 0;
    const char *p = input;
    unsigned char *edid_data;

    while (*p) {
        if (strncmp(p, "0x", 2) == 0) {
            count++;
            p += 4;
        } else {
            p++;
        }
    }

    edid_data = malloc((size_t)count);
    if (!edid_data)
        return NULL;

    p = input;
    for (int index = 0; *p && index < count;) {
        if (strncmp(p, "0x", 2) == 0) {
            unsigned int value;
            if (sscanf(p, "0x%2X", &value) != 1) {
                free(edid_data);
                return NULL;
            }
            edid_data[index++] = (unsigned char)value;
            p += 4;
        } else {
            p++;
        }
    }

    *out_size = count;
    return edid_data;
}

static unsigned char *load_edid_from_file(const char *filename, int *size)
{
    FILE *file = fopen(filename, "rb");
    long file_size;
    size_t bytes_read;
    unsigned char *data;
    unsigned char *edid_data;

    if (!file) {
        perror("open EDID file failed");
        return NULL;
    }

    if (fseek(file, 0, SEEK_END) != 0 ||
        (file_size = ftell(file)) < 0 || file_size > INT_MAX ||
        fseek(file, 0, SEEK_SET) != 0) {
        fprintf(stderr, "读取 EDID 文件大小失败\n");
        fclose(file);
        return NULL;
    }

    data = malloc((size_t)file_size + 1);
    if (!data) {
        fprintf(stderr, "malloc failed\n");
        fclose(file);
        return NULL;
    }

    bytes_read = fread(data, 1, (size_t)file_size, file);
    fclose(file);
    if (bytes_read != (size_t)file_size) {
        fprintf(stderr, "Incomplete file reading\n");
        free(data);
        return NULL;
    }
    data[bytes_read] = '\0';

    edid_data = parse_hex_string((const char *)data, size);
    free(data);
    return edid_data;
}

static int set_edid(const char *dev, unsigned char *edid_data, int blocks)
{
    struct v4l2_edid edid;
    int fd = open(dev, O_RDWR);
    int ret;

    if (fd < 0) {
        perror("open device failed");
        return -1;
    }

    memset(&edid, 0, sizeof(edid));
    edid.pad = 0;
    edid.start_block = 0;
    edid.blocks = blocks;
    edid.edid = edid_data;

    ret = ioctl(fd, VIDIOC_S_EDID, &edid);
    if (ret < 0)
        perror("set EDID failed");
    else
        printf("set EDID success: %s\n", dev);

    close(fd);
    return ret < 0 ? -1 : 0;
}

int main(int argc, char *argv[])
{
    const char *dev = "/dev/video0";
    const char *edid_file = NULL;
    const char *edid_fmt = NULL;
    const char *generated_resolution = NULL;
    int print_edid = 0;
    int set_default_edid = 0;
    int dry_run = 0;
    int opt;

    while ((opt = getopt(argc, argv, "d:f:s:r:phmn")) != -1) {
        switch (opt) {
        case 'd':
            dev = optarg;
            break;
        case 'f':
            edid_file = optarg;
            break;
        case 's':
            edid_fmt = optarg;
            break;
        case 'r':
            generated_resolution = optarg;
            break;
        case 'p':
            print_edid = 1;
            break;
        case 'm':
            set_default_edid = 1;
            break;
        case 'n':
            dry_run = 1;
            break;
        case 'h':
            print_usage(argv[0]);
            return 0;
        default:
            print_usage(argv[0]);
            return 1;
        }
    }

    if (print_edid) {
        printf("use cmd: v4l2-ctl -d %s --get-edid\n", dev);
        execlp("v4l2-ctl", "v4l2-ctl", "-d", dev, "--get-edid",
               (char *)NULL);
        perror("execute v4l2-ctl failed");
        return 1;
    }

    if (set_default_edid)
        return set_edid(dev, default_edid_data, 2);

    if (edid_file) {
        unsigned char *edid_data;
        int edid_size;
        int blocks;
        int ret;

        edid_data = load_edid_from_file(edid_file, &edid_size);
        if (!edid_data)
            return 1;
        if (edid_size == 0 || edid_size % PER_BLOCK != 0) {
            fprintf(stderr, "EDID 大小必须是 128 字节的整数倍，当前为 %d 字节\n",
                    edid_size);
            free(edid_data);
            return 1;
        }

        blocks = edid_size / PER_BLOCK;
        printf("set EDID data: %d block(s), %d bytes\n", blocks, edid_size);
        ret = set_edid(dev, edid_data, blocks);
        free(edid_data);
        return ret < 0 ? 1 : 0;
    }

    if (generated_resolution) {
        unsigned char generated_edid[MAX_EDID_SIZE];
        struct video_timing timing;
        unsigned int requested_width;
        unsigned int requested_height;
        unsigned int requested_frame_rate;
        unsigned int actual_refresh_millihz;
        unsigned int generated_blocks;
        const char *sync_polarity;

        if (parse_video_mode(generated_resolution, &requested_width,
                             &requested_height, &requested_frame_rate) < 0) {
            fprintf(stderr,
                    "无效视频模式: %s，请使用 WIDTHxHEIGHT[@FPS] 格式，帧率范围 1-240\n",
                    generated_resolution);
            return 1;
        }

        if (generate_edid_array(requested_width, requested_height,
                                requested_frame_rate,
                                generated_edid, &timing,
                                &generated_blocks) < 0) {
            fprintf(stderr, "无法为 %ux%u@%u 生成有效 EDID，可能超出当前链路或 EDID 时钟范围\n",
                    requested_width, requested_height, requested_frame_rate);
            return 1;
        }

        actual_refresh_millihz =
            (unsigned int)((uint64_t)timing.pixel_clock_khz * 1000000ULL /
                           ((uint64_t)timing.htotal * timing.vtotal));
        switch (timing.dtd_flags & 0x06) {
        case 0x06:
            sync_polarity = "+hsync +vsync";
            break;
        case 0x04:
            sync_polarity = "-hsync +vsync";
            break;
        case 0x02:
            sync_polarity = "+hsync -vsync";
            break;
        default:
            sync_polarity = "-hsync -vsync";
            break;
        }
        printf("生成 %s EDID: 请求 %ux%u@%u，实际 %ux%u @ %u.%03u fps\n",
               timing.cea_vic ? "CTA-861" : "CVT-RB",
               requested_width, requested_height, requested_frame_rate,
               timing.hdisplay, timing.vdisplay,
               actual_refresh_millihz / 1000,
               actual_refresh_millihz % 1000);
        printf("Modeline: %.2f MHz  %u %u %u %u  %u %u %u %u %s\n",
               timing.pixel_clock_khz / 1000.0,
               timing.hdisplay, timing.hsync_start, timing.hsync_end,
               timing.htotal, timing.vdisplay, timing.vsync_start,
               timing.vsync_end, timing.vtotal,
               sync_polarity);
        printf("EDID: %u block(s), %u bytes%s\n",
               generated_blocks, generated_blocks * PER_BLOCK,
               timing.cea_vic ? ", native CTA VIC present" : "");
        if (timing.cea_vic)
            printf("CTA VIC: %u\n", timing.cea_vic);
        print_edid_array(generated_edid, generated_blocks);

        if (dry_run)
            return 0;
        return set_edid(dev, generated_edid, generated_blocks) < 0 ? 1 : 0;
    }

    if (edid_fmt) {
        for (size_t i = 0; i < sizeof(edid_table) / sizeof(edid_table[0]); ++i) {
            if (strcmp(edid_fmt, edid_table[i].fmt) == 0)
                return set_edid(dev, edid_table[i].data_ptr,
                                edid_table[i].blocks) < 0 ? 1 : 0;
        }
        fprintf(stderr, "没有内置 EDID: %s\n", edid_fmt);
        return 1;
    }

    if (dry_run) {
        fprintf(stderr, "-n 必须与 -r WIDTHxHEIGHT 一起使用\n");
        return 1;
    }

    print_usage(argv[0]);
    return 0;
}
