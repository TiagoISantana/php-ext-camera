// camera.c

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_camera.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <glob.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <linux/videodev2.h>
#include <jpeglib.h>
#include <errno.h>

// forward declarations
PHP_MINIT_FUNCTION(camera);

// --- Arginfo ---
ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_camera_getDevices, 0, 0, IS_ARRAY, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_camera_snapshot, 0, 1, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, device, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, width, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, height, IS_LONG, 0)
ZEND_END_ARG_INFO()

ZEND_BEGIN_ARG_WITH_RETURN_TYPE_INFO_EX(arginfo_camera_detect_motion, 0, 2, _IS_BOOL, 0)
    ZEND_ARG_TYPE_INFO(0, device, IS_STRING, 0)
    ZEND_ARG_TYPE_INFO(0, threshold_percent, IS_DOUBLE, 0)
    ZEND_ARG_TYPE_INFO(0, width, IS_LONG, 0)
    ZEND_ARG_TYPE_INFO(0, height, IS_LONG, 0)
ZEND_END_ARG_INFO()

// --- Helpers for V4L2 capture using mmap ---
struct buffer {
    void   *start;
    size_t length;
};

static int xioctl(int fd, int request, void *arg) {
    int r;
    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && EINTR == errno);
    return r;
}

// capture a single frame (YUYV or MJPEG), return pointer and length (allocated)
static int capture_frame(const char *devname, int width, int height, void **outbuf, size_t *outlen, int *is_mjpeg)
{
    int fd = -1;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;
    struct buffer *buffers = NULL;
    unsigned int i;
    int ret = -1;

    fd = open(devname, O_RDWR | O_NONBLOCK, 0);
    if (fd == -1) {
        goto cleanup;
    }

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    // We'll try to ask for MJPEG first, then fallback to YUYV
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field = V4L2_FIELD_ANY;

    if (xioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
        // fallback to YUYV
        fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
        if (xioctl(fd, VIDIOC_S_FMT, &fmt) == -1) {
            goto cleanup;
        }
    }

    // detect if device actually gave MJPEG format
    *is_mjpeg = (fmt.fmt.pix.pixelformat == V4L2_PIX_FMT_MJPEG);

    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd, VIDIOC_REQBUFS, &req) == -1) {
        goto cleanup;
    }

    buffers = calloc(req.count, sizeof(*buffers));
    if (!buffers) goto cleanup;

    for (i = 0; i < req.count; ++i) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) == -1) goto cleanup;
        buffers[i].length = buf.length;
        buffers[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (buffers[i].start == MAP_FAILED) goto cleanup;
    }

    // queue the buffers
    for (i = 0; i < req.count; ++i) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        if (xioctl(fd, VIDIOC_QBUF, &buf) == -1) goto cleanup;
    }

    // start streaming
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) == -1) goto cleanup;

    // wait and dequeue one buffer
    fd_set fds;
    struct timeval tv;
    int r;

    FD_ZERO(&fds);
    FD_SET(fd, &fds);
    tv.tv_sec = 2;
    tv.tv_usec = 0;

    r = select(fd + 1, &fds, NULL, NULL, &tv);
    if (r <= 0) goto cleanup;

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd, VIDIOC_DQBUF, &buf) == -1) goto cleanup;

    // copy buffer into newly allocated memory to return to PHP
    *outlen = buf.bytesused;
    *outbuf = malloc(*outlen);
    if (!*outbuf) goto cleanup;
    memcpy(*outbuf, buffers[buf.index].start, *outlen);

    // requeue and stop
    if (xioctl(fd, VIDIOC_QBUF, &buf) == -1) goto cleanup;
    if (xioctl(fd, VIDIOC_STREAMOFF, &type) == -1) goto cleanup;

    ret = 0; // success

cleanup:
    if (buffers) {
        for (i = 0; i < req.count; ++i) {
            if (buffers[i].start && buffers[i].length)
                munmap(buffers[i].start, buffers[i].length);
        }
        free(buffers);
    }
    if (fd != -1) close(fd);
    return ret;
}

// --- JPEG encoding from MJPEG data or from YUYV raw conversion ---
static int yuyv_to_jpeg(const unsigned char *yuyv, int width, int height, unsigned char **outjpeg, unsigned long *outlen)
{
    int row_stride = width * 3;
    JSAMPLE *rgb_buffer = malloc(width * height * 3);
    if (!rgb_buffer) return -1;

    unsigned char *p = (unsigned char *)yuyv;
    unsigned char *rgbp = (unsigned char *)rgb_buffer;

    for (int i = 0; i < width * height; i += 2) {
        int y0 = p[0];
        int u  = p[1];
        int y1 = p[2];
        int v  = p[3];

        int c0 = y0 - 16;
        int c1 = y1 - 16;
        int d = u - 128;
        int e = v - 128;

        int r0 = (298 * c0 + 409 * e + 128) >> 8;
        int g0 = (298 * c0 - 100 * d - 208 * e + 128) >> 8;
        int b0 = (298 * c0 + 516 * d + 128) >> 8;

        int r1 = (298 * c1 + 409 * e + 128) >> 8;
        int g1 = (298 * c1 - 100 * d - 208 * e + 128) >> 8;
        int b1 = (298 * c1 + 516 * d + 128) >> 8;

        if (r0 < 0) r0 = 0; if (r0 > 255) r0 = 255;
        if (g0 < 0) g0 = 0; if (g0 > 255) g0 = 255;
        if (b0 < 0) b0 = 0; if (b0 > 255) b0 = 255;

        if (r1 < 0) r1 = 0; if (r1 > 255) r1 = 255;
        if (g1 < 0) g1 = 0; if (g1 > 255) g1 = 255;
        if (b1 < 0) b1 = 0; if (b1 > 255) b1 = 255;

        *rgbp++ = (unsigned char) r0;
        *rgbp++ = (unsigned char) g0;
        *rgbp++ = (unsigned char) b0;
        *rgbp++ = (unsigned char) r1;
        *rgbp++ = (unsigned char) g1;
        *rgbp++ = (unsigned char) b1;

        p += 4;
    }

    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPROW row_pointer[1];

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    unsigned char *outbuffer = NULL;
    unsigned long outsize = 0;
    jpeg_mem_dest(&cinfo, &outbuffer, &outsize);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 85, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height) {
        row_pointer[0] = &rgb_buffer[cinfo.next_scanline * row_stride];
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    free(rgb_buffer);

    if (!outbuffer) return -1;
    *outjpeg = outbuffer;
    *outlen = outsize;
    return 0;
}

static int mjpeg_clone(const unsigned char *data, size_t len, unsigned char **out, unsigned long *outlen)
{
    unsigned char *buf = malloc(len);
    if (!buf) return -1;
    memcpy(buf, data, len);
    *out = buf;
    *outlen = (unsigned long)len;
    return 0;
}

// --- PHP class methods ---
PHP_METHOD(Camera, getDevices)
{
    ZEND_PARSE_PARAMETERS_NONE();

    array_init_size(return_value, 4);

    glob_t globbuf;
    int ret = glob("/dev/video*", 0, NULL, &globbuf);
    if (ret == 0) {
        for (size_t i = 0; i < globbuf.gl_pathc; ++i) {
            add_next_index_string(return_value, globbuf.gl_pathv[i]);
        }
    }
    globfree(&globbuf);
}

PHP_METHOD(Camera, snapshot)
{
    char *device = NULL;
    size_t device_len = 0;
    zend_long width = 640;
    zend_long height = 480;

    ZEND_PARSE_PARAMETERS_START(1, 3)
        Z_PARAM_STRING(device, device_len)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(width)
        Z_PARAM_LONG(height)
    ZEND_PARSE_PARAMETERS_END();

    void *frame = NULL;
    size_t framelen = 0;
    int is_mjpeg = 0;
    if (capture_frame(device, (int)width, (int)height, &frame, &framelen, &is_mjpeg) != 0) {
        if (frame) free(frame);
        zend_throw_exception(NULL, "Failed to capture frame from device", 0);
        RETURN_FALSE;
    }

    unsigned char *jpeg = NULL;
    unsigned long jpeglen = 0;

    if (is_mjpeg) {
        if (mjpeg_clone((unsigned char *)frame, framelen, &jpeg, &jpeglen) != 0) {
            free(frame);
            zend_throw_exception(NULL, "Failed to clone MJPEG frame", 0);
            RETURN_FALSE;
        }
    } else {
        if (yuyv_to_jpeg((unsigned char *)frame, (int)width, (int)height, &jpeg, &jpeglen) != 0) {
            free(frame);
            zend_throw_exception(NULL, "Failed to convert frame to JPEG", 0);
            RETURN_FALSE;
        }
    }

    RETVAL_STRINGL((char *)jpeg, (size_t)jpeglen);
    free(jpeg);
    free(frame);
}

PHP_METHOD(Camera, detectMotion)
{
    char *device = NULL;
    size_t device_len = 0;
    double threshold = 10.0;
    zend_long width = 320;
    zend_long height = 240;

    ZEND_PARSE_PARAMETERS_START(2, 4)
        Z_PARAM_STRING(device, device_len)
        Z_PARAM_DOUBLE(threshold)
        Z_PARAM_OPTIONAL
        Z_PARAM_LONG(width)
        Z_PARAM_LONG(height)
    ZEND_PARSE_PARAMETERS_END();

    void *f1 = NULL, *f2 = NULL;
    size_t l1 = 0, l2 = 0;
    int is_mjpeg1 = 0, is_mjpeg2 = 0;

    if (capture_frame(device, (int)width, (int)height, &f1, &l1, &is_mjpeg1) != 0 ||
        capture_frame(device, (int)width, (int)height, &f2, &l2, &is_mjpeg2) != 0) {
        if (f1) free(f1);
        if (f2) free(f2);
        zend_throw_exception(NULL, "Failed to capture frames for motion detection", 0);
        RETURN_FALSE;
    }

    unsigned char *jpeg1 = NULL, *jpeg2 = NULL;
    unsigned long jl1 = 0, jl2 = 0;

    if (is_mjpeg1) {
        if (mjpeg_clone((unsigned char *)f1, l1, &jpeg1, &jl1) != 0) { free(f1); free(f2); zend_throw_exception(NULL, "Failed to encode frame",0); RETURN_FALSE; }
    } else {
        if (yuyv_to_jpeg((unsigned char *)f1, (int)width, (int)height, &jpeg1, &jl1) != 0) { free(f1); free(f2); zend_throw_exception(NULL, "Failed to encode frame",0); RETURN_FALSE; }
    }

    if (is_mjpeg2) {
        if (mjpeg_clone((unsigned char *)f2, l2, &jpeg2, &jl2) != 0) { free(jpeg1); free(f1); free(f2); zend_throw_exception(NULL, "Failed to encode frame",0); RETURN_FALSE; }
    } else {
        if (yuyv_to_jpeg((unsigned char *)f2, (int)width, (int)height, &jpeg2, &jl2) != 0) { free(jpeg1); free(f1); free(f2); zend_throw_exception(NULL, "Failed to encode frame",0); RETURN_FALSE; }
    }

    size_t minlen = jl1 < jl2 ? jl1 : jl2;
    size_t diffs = 0;
    for (size_t i = 0; i < minlen; ++i) {
        if (jpeg1[i] != jpeg2[i]) diffs++;
    }
    diffs += (jl1 > jl2) ? (jl1 - jl2) : (jl2 - jl1);
    double percent = ((double)diffs / (double)( (jl1 + jl2) / 2.0 )) * 100.0;

    free(jpeg1);
    free(jpeg2);
    free(f1);
    free(f2);

    RETURN_BOOL(percent >= threshold);
}

// --- Methods table & module entry ---
static const zend_function_entry camera_methods[] = {
    PHP_ME(Camera, getDevices, arginfo_camera_getDevices, ZEND_ACC_PUBLIC)
    PHP_ME(Camera, snapshot, arginfo_camera_snapshot, ZEND_ACC_PUBLIC)
    PHP_ME(Camera, detectMotion, arginfo_camera_detect_motion, ZEND_ACC_PUBLIC)
    PHP_FE_END
};

// declare the class entry pointer
zend_class_entry *camera_ce_ptr;

// define MINIT before module entry so functions are visible
PHP_MINIT_FUNCTION(camera)
{
    zend_class_entry ce;
    INIT_CLASS_ENTRY(ce, "Camera", camera_methods);
    camera_ce_ptr = zend_register_internal_class(&ce);
    return SUCCESS;
}

zend_module_entry camera_module_entry = {
    STANDARD_MODULE_HEADER,
    "camera",
    NULL, /* functions are NULL because we register class methods */
    PHP_MINIT(camera), /* MINIT */
    NULL, /* MSHUTDOWN */
    NULL, /* RINIT */
    NULL, /* RSHUTDOWN */
    NULL, /* MINFO */
    PHP_CAMERA_VERSION,
    STANDARD_MODULE_PROPERTIES
};

#ifdef COMPILE_DL_CAMERA
ZEND_GET_MODULE(camera)
#endif