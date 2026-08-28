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
    printf("\n标准 4K 模式自动使用 CTA-861 时序和 256 字节 HDMI 2.0 扩展块。\n");

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

    if (parsed_width < 320 || parsed_width > 4095 ||
        parsed_height < 200 || parsed_height > 4095 ||
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

    if (timing->htotal > 4095 || timing->vtotal > 4095)
        return -1;

    pixel_clock_khz = (uint64_t)timing->htotal * 1000000ULL / hperiod_ns;
    pixel_clock_khz -= pixel_clock_khz % CVT_CLOCK_STEP_KHZ;
    if (!pixel_clock_khz || pixel_clock_khz > 655350)
        return -1;

    timing->pixel_clock_khz = (unsigned int)pixel_clock_khz;
    return 0;
}

static int generate_cea_timing(unsigned int width, unsigned int height,
                               unsigned int frame_rate,
                               struct video_timing *timing)
{
    static const struct cea_mode modes[] = {
        {3840, 2160, 24, 297000, 1276, 88, 296, 8, 10, 72, 93},
        {3840, 2160, 25, 297000, 1056, 88, 296, 8, 10, 72, 94},
        {3840, 2160, 30, 297000,  176, 88, 296, 8, 10, 72, 95},
        {3840, 2160, 50, 594000, 1056, 88, 296, 8, 10, 72, 96},
        {3840, 2160, 60, 594000,  176, 88, 296, 8, 10, 72, 97},
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
        timing->dtd_flags = 0x1e; /* Positive HSync, positive VSync. */
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

static int generate_native_compatible_4k_edid(
    unsigned int native_vic, unsigned char edid[MAX_EDID_SIZE])
{
    unsigned char *extension;
    size_t position;
    int found_native_vic = 0;

    if (!edid || native_vic < 93 || native_vic > 97 ||
        sizeof(default_edid_data) < MAX_EDID_SIZE)
        return -1;

    /* Start from the board's original RK-UHD EDID. It advertises 4K50/60
     * through a YCbCr 4:2:0 Video Data Block and limits the legacy HDMI VSDB
     * to 340 MHz, which matches the receiver's known-good compatibility path.
     */
    memcpy(edid, default_edid_data, MAX_EDID_SIZE);
    extension = edid + PER_BLOCK;

    if (extension[0] != 0x02 || extension[2] < 4 ||
        extension[2] >= PER_BLOCK)
        return -1;

    position = 4;
    while (position < extension[2]) {
        unsigned int tag = extension[position] >> 5;
        unsigned int length = extension[position] & 0x1f;

        if (position + 1 + length > extension[2])
            return -1;
        if (tag == 2) {
            for (unsigned int i = 0; i < length; ++i) {
                unsigned char vic = extension[position + 1 + i] & 0x7f;

                extension[position + 1 + i] = vic;
                if (vic == native_vic) {
                    extension[position + 1 + i] = 0x80 | vic;
                    found_native_vic = 1;
                }
            }
        }
        position += 1 + length;
    }

    if (!found_native_vic)
        return -1;

    edid[127] = calculate_block_checksum(edid);
    extension[127] = calculate_block_checksum(extension);
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
        0x6d, 0x03, 0x0c, 0x00, 0x10, 0x00, 0x00,
        0x44, 0x20, 0x00, 0x60, 0x03, 0x02, 0x01
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
    size_t position = 4;

    if (!extension || native_vic > 127 || (native_vic && native_dtd))
        return -1;

    memset(extension, 0, PER_BLOCK);
    extension[0] = 0x02; /* CTA extension tag */
    extension[1] = 0x03; /* CTA-861 revision 3 */
    extension[3] = 0xf0; /* Basic audio, YCbCr 4:4:4 and 4:2:2 */

    if (native_vic) {
        extension[position++] = 0x44; /* Video Data Block, four SVDs */
        extension[position++] = 0x80 | native_vic;
        extension[position++] = 16;   /* 1920x1080p60 fallback */
        extension[position++] = 4;    /* 1280x720p60 fallback */
        extension[position++] = 1;    /* 640x480p60 fallback */
    } else {
        /* Custom modes have no CTA VIC. Do not incorrectly mark VIC 16 as
         * native: the custom DTD below is the preferred/native timing.
         */
        extension[position++] = 0x43; /* Video Data Block, three fallbacks */
        extension[position++] = 16;
        extension[position++] = 4;
        extension[position++] = 1;
    }

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
    static const unsigned char chromaticity[10] = {
        0x0a, 0xcf, 0x74, 0xa3, 0x57, 0x4c, 0xb0, 0x23, 0x09, 0x48
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

    if (timing.cea_vic >= 93 && timing.cea_vic <= 97) {
        if (generate_native_compatible_4k_edid(timing.cea_vic, edid) < 0)
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
    edid[8] = 0x49;       /* Manufacturer: RKP */
    edid[9] = 0x70;
    edid[10] = 0x88;     /* Product code: 0x3588 */
    edid[11] = 0x35;
    edid[12] = 0x01;     /* Serial number */
    edid[16] = 0x01;     /* Manufacture week */
    edid[17] = 0x24;     /* Manufacture year: 2026 */
    edid[18] = 0x01;     /* EDID 1.4 */
    edid[19] = 0x04;
    edid[20] = 0x80;     /* Digital input */
    edid[23] = 0x78;     /* Gamma 2.2 */
    edid[24] = 0x0a;     /* Preferred timing is in descriptor 1 */
    memcpy(edid + 25, chromaticity, sizeof(chromaticity));
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
                          is_cea_timing ? "CTA-4K" : "AUTO-CVT-RB");

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
            fprintf(stderr, "无法为 %ux%u@%u 生成有效 EDID，可能超出 EDID 时钟范围\n",
                    requested_width, requested_height, requested_frame_rate);
            return 1;
        }

        actual_refresh_millihz =
            (unsigned int)((uint64_t)timing.pixel_clock_khz * 1000000ULL /
                           ((uint64_t)timing.htotal * timing.vtotal));
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
               timing.dtd_flags == 0x1e ? "+hsync +vsync" :
                                           "+hsync -vsync");
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
