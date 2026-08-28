#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <getopt.h>
#include <errno.h>
#include <ctype.h>

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

#define PER_BLOCK (128)

void print_usage(const char *program_name) {
    printf("用法: %s [选项]\n", program_name);
    printf("选项:\n");
    printf("  -d <设备>   指定视频设备节点 (默认: /dev/video0)\n");
    printf("  -f <文件>   从文件加载EDID数据\n");
    printf("  -p          打印EDID数据,不设置\n");
    printf("  -m          设置默认edid\n");
    printf("  -h          显示此帮助信息\n");
    printf("\n示例:\n");
    printf("  %s -d /dev/video0             # 设置默认EDID到video0设备\n", program_name);
    printf("  %s -f my_edid.txt             # 从文件加载EDID并设置\n", program_name);
    printf("  %s -p                         # 打印/dev/video0的EDID数据\n", program_name);
    printf("  %s -d /dev/video0 -f edid.txt # 设置指定设备的EDID\n", program_name);
    printf("  %s -s 1920x1080            # 设置已有分辨率EDID\n", program_name);

    printf("当前支持主动配置分辨率：\n");
    for (int i = 0; i < (sizeof(edid_table) / sizeof(struct edid_map)); ++i) {
        printf("%s\n", edid_table[i].fmt);
    }
}

int hexstr_to_array(const char *src, unsigned char *out, int max)
{
    int n = 0;
    while (*src && n < max) {
        while (*src && !isxdigit((unsigned char)*src)) ++src;
        if (!*src) break;

        unsigned byte;
        if (sscanf(src, "%2x", &byte) != 1) break;
        out[n++] = (unsigned char)byte;

        src += 2;
    }
    return n;
}

unsigned char* parse_hex_string(const char* input, int* out_size) {
    int count = 0;
    const char* p = input;
    
    while (*p) {
        if (strncmp(p, "0x", 2) == 0) {
            count++;
            p += 4;
        } else {
            p++;
        }
    }
    
    unsigned char* edid_data = malloc(count);
    if (!edid_data) return NULL;
    
    p = input;
    int index = 0;
    while (*p && index < count) {
        if (strncmp(p, "0x", 2) == 0) {
            unsigned int value;
            sscanf(p, "0x%2X", &value);
            edid_data[index++] = (unsigned char)value;
            p += 4;
        } else {
            p++;
        }
    }
    
    *out_size = count;
    return edid_data;
}



unsigned char *load_edid_from_file(const char *filename, int *size) {
    FILE *file = fopen(filename, "rb");
    if (!file) {
        perror("Unabled open file!");
        return NULL;
    }
    printf("open file %s success\n", filename);

    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);

    printf("file size %d \n", file_size);

    unsigned char *data = malloc(file_size);
    if (!data) {
        fprintf(stderr, "malloc failed\n");
        fclose(file);
        return NULL;
    }
    
    int bytes_read = fread(data, 1, file_size, file);
    fclose(file);
    
    if (bytes_read != file_size) {
        fprintf(stderr, "Incomplete file reading\n");
        free(data);
        return NULL;
    }
    
    unsigned char* edid_data = parse_hex_string(data, size);
    
    if (edid_data) {
        printf("Analysis successful, data size: %d byte\n", *size);
    
        for (int i =0; i < *size; ++i)
        {
            if (i % 8 == 0) printf("\n");
            if (i == 128)   printf("\n");
            printf("0x%02X ", edid_data[i]);
        }
    }
    printf("\n");

    return edid_data;
}

int set_edid(const char* dev, unsigned char* edid_data, int blocks)
{
    int fd;
    int ret;
    struct v4l2_edid edid;

    fd = open(dev, O_RDWR);
    if (fd < 0) {
        perror("open device failed!");
        return -1;
    }

    memset(&edid, 0, sizeof(edid));
    edid.pad = 0;
    edid.start_block = 0;
    edid.blocks = blocks;
    edid.edid = edid_data;

    ret = ioctl(fd, VIDIOC_S_EDID, &edid);
    if (ret < 0) {
        perror("set edid failed");
        close(fd);
        return -1;
    }

    printf("set edid success!\n");

    close(fd);

    return 0;
}

int main(int argc, char *argv[]) {
    int edid_size;
    const char *dev = "/dev/video0";
    int ret;
    int opt;
    int blocks;
    int print_edid = 0, set_def_edid = 0;
    unsigned char* edid_data;
    const char *edid_file = NULL;
    const char *edid_fmt = NULL;
    int i = 0;

    while ((opt = getopt(argc, argv, "d:f:s:phm")) != -1) {
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
            case 'p':
                print_edid = 1;
                break;
            case 'm':
                set_def_edid = 1;
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
        printf("use cmd \"v4l2-ctl -d /dev/video0 --get-edid\"\nNow video0 device edid:\n");
        system("v4l2-ctl -d /dev/video0 --get-edid");
        return 0;
    }

    if (set_def_edid) {
        printf("Set the default device node /dev/video0 as the default edid\n");
        set_edid(dev, default_edid_data, 2);
        return 0;
    }

    if (edid_file) {
        printf("Used device: %s\n", dev);
        edid_data = load_edid_from_file(edid_file, &edid_size);
        blocks = edid_size / PER_BLOCK;

        printf("set edid data, total %d block (%zu byte)\n", blocks, edid_size);
        set_edid(dev, edid_data, blocks);
        return 0;
    }

    if (edid_fmt) {
        for (i = 0; i < (sizeof(edid_table) / sizeof(struct edid_map)); ++i) {
            if (!strncmp(edid_fmt, edid_table[i].fmt, sizeof(edid_fmt)))
                return set_edid(dev, edid_table[i].data_ptr, edid_table[i].blocks);
        }
        return 0;
    }

    print_usage(NULL);

    return 0;
}


