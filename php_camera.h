#ifndef PHP_CAMERA_H
#define PHP_CAMERA_H

#include "php.h"

extern zend_module_entry camera_module_entry;
#define phpext_camera_ptr &camera_module_entry

#define PHP_CAMERA_VERSION "0.2.0"

extern zend_class_entry *camera_ce_ptr;

PHP_METHOD(Camera, getDevices);
PHP_METHOD(Camera, snapshot);
PHP_METHOD(Camera, detectMotion);

#endif
