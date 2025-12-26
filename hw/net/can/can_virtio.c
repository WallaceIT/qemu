/*
 * Virtio CAN Device
 *
 * Copyright (C) 2023 OpenSynergy GmbH
 * Copyright (C) 2025 Francesco Valla <francesco@valla.it>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "qemu/iov.h"
#include "standard-headers/linux/virtio_can.h"
#include "hw/virtio/virtio-can.h"
#include "hw/virtio/virtio.h"

static bool virtio_can_started(VirtIOCAN *vcan)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(vcan);

    return (vdev->status & VIRTIO_CONFIG_S_DRIVER_OK) &&
            vcan->ctrl_state == CAN_CS_STARTED && vdev->vm_running;
}

/*
 * Controller stop and controller start has only a local impact on the device.
 * If the state is stopped, everything received from the internal qemu CAN bus
 * is dropped as the controller is inactive. If the state is started, messages
 * are forwarded in both directions.
 */
static void virtio_can_controller_stop(VirtIOCAN *vcan)
{
    vcan->ctrl_state = CAN_CS_STOPPED;
}

static void virtio_can_controller_start(VirtIOCAN *vcan)
{
    vcan->ctrl_state = CAN_CS_STARTED;
}

static int virtio_can_set_status(VirtIODevice *vdev, uint8_t status)
{
    VirtIOCAN *can = VIRTIO_CAN(vdev);

    if (!vdev->vm_running) {
        return 0;
    }

    vdev->status = status;

    if ((status & VIRTIO_CONFIG_S_DRIVER_OK) == 0) {
        virtio_can_controller_stop(can);
    }
    return 0;
}

/*
 * Process DEVICE_QUEUE_TX. Driver side view! The device receives here
 * so we forward messages received from virtio CAN to the qemu CAN bus
 */
static void virtio_can_tx_cb(VirtIODevice *vdev, VirtQueue *vqueue)
{
    VirtIOCAN *vcan = VIRTIO_CAN(vdev);
    VirtQueueElement *element;
    struct virtio_can_tx_out *request;
    struct virtio_can_tx_in response;
    qemu_can_frame qemu_frame;
    size_t req_size;
    size_t resp_size;
    uint32_t flags;
    uint16_t msg_type;
    uint16_t sdu_len;

    for (;;) {
        element = virtqueue_pop(vqueue, sizeof(VirtQueueElement));
        if (element == NULL) {
            break;
        }

        /* Device => Driver part */
        resp_size = iov_size(element->in_sg, element->in_num);
        if (resp_size < sizeof(response.result)) {
            virtio_error(vdev, "Wrong response size (%zu bytes)\n", resp_size);
            goto on_failure_no_result;
        }

        response.result = VIRTIO_CAN_RESULT_NOT_OK;

        if (!virtio_can_started(vcan)) {
            goto on_failure_no_free;
        }

        /* Driver => Device part */
        req_size = iov_size(element->out_sg, element->out_num);
        if (req_size < sizeof(struct virtio_can_tx_out)) {
            virtio_error(vdev, "TX: Message too small for header\n");
            goto on_failure_no_free;
        }

        request = g_malloc(req_size);
        iov_to_buf(element->out_sg, element->out_num, 0, request, req_size);

        msg_type = le16_to_cpu(request->msg_type);
        if (msg_type != VIRTIO_CAN_TX) {
            virtio_error(vdev, "TX: Message type 0x%x unknown\n", msg_type);
            goto on_failure;
        }

        flags = le32_to_cpu(request->flags);
        sdu_len = le16_to_cpu(request->length);
        if (flags & VIRTIO_CAN_FLAGS_FD) {
            if (sdu_len > 64) {
                virtio_error(vdev, "TX: Truncate sdu_len from %u to 64\n", sdu_len);
                sdu_len = 64;
            }
        } else {
            if (sdu_len > 8) {
                virtio_error(vdev, "TX: Truncate sdu_len from %u to 8\n", sdu_len);
                sdu_len = 8;
            }
        }
        if (req_size < offsetof(struct virtio_can_tx_out, sdu) + sdu_len) {
            virtio_error(vdev, "TX: Message too small for payload\n");
            goto on_failure;
        }

        /*
         * Copy Virtio frame structure to qemu frame structure and
         * check while doing this whether the frame type was negotiated
         */
        qemu_frame.can_id = le32_to_cpu(request->can_id);
        if (flags & VIRTIO_CAN_FLAGS_EXTENDED) {
            qemu_frame.can_id &= QEMU_CAN_EFF_MASK;
            qemu_frame.can_id |= QEMU_CAN_EFF_FLAG;
        } else {
            qemu_frame.can_id &= QEMU_CAN_SFF_MASK;
        }

        if (flags & VIRTIO_CAN_FLAGS_RTR) {
            if (!virtio_vdev_has_feature(vdev, VIRTIO_CAN_F_CAN_CLASSIC) ||
                !virtio_vdev_has_feature(vdev, VIRTIO_CAN_F_RTR_FRAMES)) {
                virtio_error(vdev, "TX: RTR frames not negotiated\n");
                goto on_failure;
            }
            qemu_frame.can_id |= QEMU_CAN_RTR_FLAG;
        }

        if (flags & VIRTIO_CAN_FLAGS_FD) {
            if (!virtio_vdev_has_feature(vdev, VIRTIO_CAN_F_CAN_FD)) {
                virtio_error(vdev, "TX: FD frames not negotiated\n");
                goto on_failure;
            }
            qemu_frame.flags |= QEMU_CAN_FRMF_TYPE_FD;
        } else {
            if (!virtio_vdev_has_feature(vdev, VIRTIO_CAN_F_CAN_CLASSIC)) {
                virtio_error(vdev, "TX: Classic frames not negotiated\n");
                goto on_failure;
            }
            qemu_frame.flags = 0;
        }

        qemu_frame.can_dlc = (uint8_t)sdu_len;
        memcpy(qemu_frame.data, request->sdu, sdu_len);

        can_bus_client_send(&vcan->bus_client, &qemu_frame, 1);

        vcan->tx_from_driver_to_bus++;

        response.result = VIRTIO_CAN_RESULT_OK;

on_failure:
        g_free(request);

on_failure_no_free:
        iov_from_buf(element->in_sg, element->in_num, 0, &response,
                     sizeof(response.result));

on_failure_no_result:
        virtqueue_push(vqueue, element, resp_size);
    }

    virtio_notify(vdev, vqueue);
}

static bool virtio_can_can_receive(CanBusClientState *client)
{
    VirtIOCAN *vcan = container_of(client, VirtIOCAN, bus_client);

    return virtio_can_started(vcan);
}

/* From qemu internal bus => virtio */
static ssize_t virtio_can_receive(CanBusClientState *client,
                                  const qemu_can_frame *frames,
                                  size_t frames_cnt)
{
    VirtIOCAN *vcan = container_of(client, VirtIOCAN, bus_client);
    VirtIODevice *vdev = &vcan->parent_obj;
    VirtQueue *vqueue = vcan->vq[DEVICE_QUEUE_RX];
    const qemu_can_frame *frame = frames;
    VirtQueueElement *element;
    g_autofree struct virtio_can_rx *can_rx = NULL;
    uint32_t flags = 0;
    uint16_t sdu_len;
    uint16_t msg_len;

    if (frames_cnt <= 0) {
        return 0;
    }

    if (!virtio_can_started(vcan)) {
        return 0;
    }

    if (frame->can_id & QEMU_CAN_ERR_FLAG) {
        if (frame->can_id & QEMU_CAN_ERR_BUSOFF) {
            vcan->busoff = true;
            virtio_can_controller_stop(vcan);
            virtio_notify_config(vdev);
        }
        return 1; /* Silently drop other error frames */
    }

    if ((frame->can_id & QEMU_CAN_RTR_FLAG) &&
        !virtio_vdev_has_feature(vdev, VIRTIO_CAN_F_RTR_FRAMES)) {
        return -1; /* Drop non-supported RTR frame */
    }

    sdu_len = frame->can_dlc;

    if (frame->flags & QEMU_CAN_FRMF_TYPE_FD) {
        if (!virtio_vdev_has_feature(vdev, VIRTIO_CAN_F_CAN_FD)) {
            return -1; /* Drop non-supported CAN FD frame */
        }

        flags |= VIRTIO_CAN_FLAGS_FD;
        if (sdu_len > 64) {
            sdu_len = 64;
        }
    } else {
        if (!virtio_vdev_has_feature(vdev, VIRTIO_CAN_F_CAN_CLASSIC)) {
            return -1; /* Drop non-supported CAN classic frame */
        }

        if (sdu_len > 8) {
            sdu_len = 8;
        }
    }

    element = virtqueue_pop(vqueue, sizeof(VirtQueueElement));
    if (element == NULL) {
        virtio_error(vdev, "RX: no virtqueue element available\n");
        virtio_can_controller_stop(vcan);
        virtio_notify_config(vdev);
        return -1;
    }

    msg_len = sizeof(struct virtio_can_rx) + sdu_len;
    can_rx = g_malloc0(msg_len);

    can_rx->msg_type = cpu_to_le16(VIRTIO_CAN_RX);
    if (frame->can_id & QEMU_CAN_EFF_FLAG) {
        flags |= VIRTIO_CAN_FLAGS_EXTENDED;
        can_rx->can_id = cpu_to_le32(frame->can_id & QEMU_CAN_EFF_MASK);
    } else {
        can_rx->can_id = cpu_to_le32(frame->can_id & QEMU_CAN_SFF_MASK);
    }
    if (frame->can_id & QEMU_CAN_RTR_FLAG) {
        flags |= VIRTIO_CAN_FLAGS_RTR;
    }

    can_rx->length = cpu_to_le16(sdu_len);
    can_rx->flags = cpu_to_le32(flags);
    memcpy(can_rx->sdu, frame->data, sdu_len);

    iov_from_buf(element->in_sg, element->in_num, 0, can_rx, msg_len);
    virtqueue_push(vqueue, element, msg_len);

    virtio_notify(vdev, vqueue);

    return 1;
}

static CanBusClientInfo virtio_can_bus_client_info = {
    .can_receive = virtio_can_can_receive,
    .receive = virtio_can_receive,
};

/* Compare with ctucan_connect_to_bus() */
int virtio_can_connect_to_bus(VirtIOCAN *vcan, CanBusState *bus)
{
    vcan->bus_client.info = &virtio_can_bus_client_info;

    if (bus == NULL) {
        return -EINVAL;
    }

    if (can_bus_insert_client(bus, &vcan->bus_client) < 0) {
        return -1;
    }

    return 0;
}

/* Compare with ctucan_disconnect() */
void virtio_can_disconnect_from_bus(VirtIOCAN *vcan)
{
    can_bus_remove_client(&vcan->bus_client);
}

void virtio_can_init(VirtIOCAN *vcan)
{
    (void)vcan;
}

/* Control message received */
static void virtio_can_ctrl_cb(VirtIODevice *vdev, VirtQueue *vqueue)
{
    VirtIOCAN *vcan = VIRTIO_CAN(vdev);
    size_t req_size;
    size_t resp_size;
    uint16_t msg_type;

    for (;;) {
        VirtQueueElement *element;
        struct virtio_can_control_out request;
        struct virtio_can_control_in response;

        element = virtqueue_pop(vqueue, sizeof(VirtQueueElement));
        if (element == NULL) {
            break;
        }

        /* Device => Driver part */
        resp_size = iov_size(element->in_sg, element->in_num);
        if (resp_size < sizeof(response.result)) {
            virtio_error(vdev, "Wrong response size (%zu bytes)\n", resp_size);
            goto on_failure_no_result;
        }

        response.result = VIRTIO_CAN_RESULT_NOT_OK;

        /* Driver => Device part */
        req_size = iov_size(element->out_sg, element->out_num);
        if (req_size < sizeof(struct virtio_can_control_out)) {
            virtio_error(vdev, "Wrong request size (%zu bytes)\n", req_size);
            goto on_failure;
        }

        iov_to_buf(element->out_sg, element->out_num, 0, &request,
                   sizeof(struct virtio_can_control_out));

        msg_type = le16_to_cpu(request.msg_type);
        switch (msg_type) {
        case VIRTIO_CAN_SET_CTRL_MODE_START:
            vcan->busoff = false;
            virtio_can_controller_start(vcan);
            response.result = VIRTIO_CAN_RESULT_OK;
            break;
        case VIRTIO_CAN_SET_CTRL_MODE_STOP:
            virtio_can_controller_stop(vcan);
            vcan->busoff = false;
            response.result = VIRTIO_CAN_RESULT_OK;
            break;
        default:
            virtio_error(vdev, "Ctrl queue: msg type 0x%" PRIX16 " unknown\n",
                         msg_type);
            break;
        }

        response.result = VIRTIO_CAN_RESULT_OK;

on_failure:
        iov_from_buf(element->in_sg, element->in_num, 0, &response,
                     sizeof(response.result));

on_failure_no_result:
        virtqueue_push(vqueue, element, resp_size);
    }

    virtio_notify(vdev, vqueue);
}

static void virtio_can_device_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOCAN *vcan = VIRTIO_CAN(dev);

    (void)errp;

    virtio_init(vdev, VIRTIO_ID_CAN, sizeof(struct virtio_can_config));
    vcan->vq[DEVICE_QUEUE_TX] = virtio_add_queue(vdev, 64, virtio_can_tx_cb);
    vcan->vq[DEVICE_QUEUE_RX] = virtio_add_queue(vdev, 64, NULL);
    vcan->vq[DEVICE_QUEUE_CTRL] = virtio_add_queue(vdev, 4, virtio_can_ctrl_cb);
}

static void virtio_can_device_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    VirtIOCAN *vcan = VIRTIO_CAN(dev);
    unsigned int qi;

    for (qi = 0; qi < DEVICE_QUEUE_CNT; qi++) {
        virtio_delete_queue(vcan->vq[qi]);
    }

    virtio_cleanup(vdev);
}

/* Device offered features */
static uint64_t virtio_can_get_features(VirtIODevice *vdev, uint64_t features,
                                        Error **errp)
{
    VirtIOCAN *vcan = VIRTIO_CAN(vdev);

    (void)errp;

    virtio_add_feature(&features, VIRTIO_F_VERSION_1);

    if (vcan->support_can_classic) {
        virtio_add_feature(&features, VIRTIO_CAN_F_CAN_CLASSIC);
        virtio_add_feature(&features, VIRTIO_CAN_F_RTR_FRAMES);
    }

    if (vcan->support_can_fd) {
        virtio_add_feature(&features, VIRTIO_CAN_F_CAN_FD);
    }

    return features;
}

static void virtio_can_get_config(VirtIODevice *vdev, uint8_t *data)
{
    struct virtio_can_config *config = (struct virtio_can_config *)data;
    VirtIOCAN *vcan = VIRTIO_CAN(vdev);

    if (vcan->busoff) {
        config->status = cpu_to_le32(VIRTIO_CAN_S_CTRL_BUSOFF);
    } else {
        config->status = cpu_to_le32(0);
    }
}

static const VMStateDescription vmstate_virtio_can = {
    .name = TYPE_VIRTIO_CAN,
    .unmigratable = 1,
};

static const Property virtio_can_properties[] = {
    DEFINE_PROP_BOOL("classic", VirtIOCAN, support_can_classic, true),
    DEFINE_PROP_BOOL("fd", VirtIOCAN, support_can_fd, true),
};

static void virtio_can_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);

    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    device_class_set_props(dc, virtio_can_properties);

    dc->vmsd = &vmstate_virtio_can;
    vdc->realize = virtio_can_device_realize;
    vdc->unrealize = virtio_can_device_unrealize;
    vdc->get_features = virtio_can_get_features;
    vdc->get_config = virtio_can_get_config;
    vdc->set_status = virtio_can_set_status;
    vdc->legacy_features = 0;
}

static const TypeInfo virtio_can_info = {
    .name = TYPE_VIRTIO_CAN,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(VirtIOCAN),
    .class_init = virtio_can_class_init,
};

static void virtio_register_types(void)
{
    type_register_static(&virtio_can_info);
}

type_init(virtio_register_types)
