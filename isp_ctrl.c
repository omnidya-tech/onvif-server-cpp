/*
 * isp_ctrl.c - VeriSilicon ISP control helper for IMX8MP
 *
 * Sends JSON commands to isp_media_server via the viv_ext_ctrl
 * V4L2 extended control and prints the JSON response.
 *
 * Usage:
 *   isp_ctrl /dev/video2 '{"id":"cproc.g.cfg","streamid":0}'
 *   echo '{"id":"ec.g.cfg","streamid":0}' | isp_ctrl /dev/video2
 *
 * Build (native):
 *   gcc -o isp_ctrl isp_ctrl.c
 *
 * Build (cross, via Yocto recipe):
 *   ${CC} ${LDFLAGS} -o isp_ctrl isp_ctrl.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#define VIV_EXT_CTRL_ID  0x0098f901
#define JSON_BUF_SIZE    65536

int main(int argc, char *argv[])
{
    if (argc < 2) {
        fprintf(stderr,
            "Usage: isp_ctrl <video_device> [json_command]\n"
            "  Sends a JSON command to the VeriSilicon ISP and prints the response.\n"
            "  If json_command is omitted, reads from stdin.\n\n"
            "Examples:\n"
            "  isp_ctrl /dev/video2 '{\"id\":\"cproc.g.cfg\",\"streamid\":0}'\n"
            "  isp_ctrl /dev/video2 '{\"id\":\"ec.g.cfg\",\"streamid\":0}'\n"
            "  isp_ctrl /dev/video2 '{\"id\":\"cproc.s.cfg\",\"streamid\":0,\"brightness\":50}'\n");
        return 1;
    }

    const char *dev = argv[1];
    char buf[JSON_BUF_SIZE];
    memset(buf, 0, sizeof(buf));

    if (argc > 2) {
        snprintf(buf, sizeof(buf), "%s", argv[2]);
    } else {
        size_t n = fread(buf, 1, sizeof(buf) - 1, stdin);
        buf[n] = '\0';
        /* Strip trailing newline */
        while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r'))
            buf[--n] = '\0';
    }

    if (buf[0] == '\0') {
        fprintf(stderr, "ERROR: empty command\n");
        return 1;
    }

    int fd = open(dev, O_RDWR);
    if (fd < 0) {
        perror("open");
        return 1;
    }

    struct v4l2_ext_control ec;
    memset(&ec, 0, sizeof(ec));
    ec.id = VIV_EXT_CTRL_ID;
    ec.size = sizeof(buf);
    ec.string = buf;

    struct v4l2_ext_controls ecs;
    memset(&ecs, 0, sizeof(ecs));
    ecs.which = V4L2_CTRL_CLASS_USER;
    ecs.count = 1;
    ecs.controls = &ec;

    /* Send JSON command to ISP */
    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &ecs) < 0) {
        perror("VIDIOC_S_EXT_CTRLS");
        close(fd);
        return 1;
    }

    /* Read JSON response from ISP */
    if (ioctl(fd, VIDIOC_G_EXT_CTRLS, &ecs) < 0) {
        perror("VIDIOC_G_EXT_CTRLS");
        close(fd);
        return 1;
    }

    printf("%s\n", buf);
    close(fd);
    return 0;
}
