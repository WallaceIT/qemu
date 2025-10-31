/*
 * Vhost-user Can virtio device
 *
 * Copyright (c) 2024 Timos Ampelikiotis <t.ampelikiotis@virtualopensystems.com>
 * Copyright (c) 2026 Francesco Valla <francesco@valla.it>
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef QEMU_VHOST_USER_CAN_H
#define QEMU_VHOST_USER_CAN_H

#include "hw/virtio/vhost-user-base.h"
#include "qom/object.h"

#define TYPE_VHOST_USER_CAN "vhost-user-can"
OBJECT_DECLARE_SIMPLE_TYPE(VHostUserCan, VHOST_USER_CAN)

struct VHostUserCan {
    VHostUserBase parent_obj;
};

#endif /* QEMU_VHOST_USER_CAN_H */
