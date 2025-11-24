PHP_ARG_ENABLE(camera, whether to enable the camera extension,
[  --enable-camera       Enable camera extension])

if test "$PHP_CAMERA" != "no"; then
  PHP_REQUIRE_CXX()
  AC_CHECK_HEADERS([linux/videodev2.h jpeglib.h sys/mman.h sys/ioctl.h], [],
    [AC_MSG_ERROR([Missing required headers for camera extension])])
  AC_CHECK_LIB([jpeg], [jpeg_std_error], [],
    [AC_MSG_ERROR([libjpeg (development) is required to build camera extension])])
  PHP_NEW_EXTENSION(camera, camera.c, $ext_shared)
  PHP_SUBST(LIBS)
fi
