/*
 * This work is licensed under the terms of the GNU GPL, version 2 or
 * (at your option) any later version. See the COPYING file in the
 * top-level directory.
 */

#include "qemu/osdep.h"

#include <sys/ioctl.h>

#include <net/if.h>
#include <linux/can.h>
#include <linux/can/error.h>
#include <linux/can/raw.h>

#include "qemu/iov.h"
#include "qemu/bswap.h"
#include "qemu/sockets.h"
#include "libvhost-user-glib.h"
#include "standard-headers/linux/virtio_can.h"
#include "qapi/error.h"

enum {
    VHOST_USER_CAN_TX_QUEUE = 0,
    VHOST_USER_CAN_RX_QUEUE = 1,
    VHOST_USER_CAN_CTRL_QUEUE = 2,
    VHOST_USER_CAN_NUM_QUEUES = 3
};

typedef struct virtio_can_config virtio_can_config;
typedef struct virtio_can_control_out virtio_can_control_out;
typedef struct virtio_can_control_in virtio_can_control_in;
typedef struct virtio_can_rx virtio_can_rx;
typedef struct virtio_can_tx_out virtio_can_tx_out;
typedef struct virtio_can_tx_in virtio_can_tx_in;

typedef struct VuCan {
    VugDev dev;
    int can_sock;
    GSource *evsrc;
    GMainLoop *loop;
    virtio_can_config config;
    gboolean started;
    gboolean has_fd;
} VuCan;

static void
vc_panic(VuDev *dev, const char *msg)
{
    VuCan *vc = container_of(dev, VuCan, dev.parent);

    if (msg) {
        g_error("Panic: %s", msg);
    }

    g_main_loop_quit(vc->loop);
}

static void
vc_socketcan_receive(VuDev *dev, int condition, void *data)
{
    VuCan *vc = data;
    VuVirtq *vq = vu_get_queue(dev, VHOST_USER_CAN_RX_QUEUE);
    struct canfd_frame cf;
    ssize_t bytes;

    bytes = read(vc->can_sock, &cf, sizeof(struct canfd_frame));
    if (bytes <= 0) {
        g_warning("SocketCAN read failed: %s", g_strerror(errno));
        return;
    }

    if (!vc->started) {
        g_debug("Controller not started, discarding frame with ID = %03X",
                cf.can_id);
        return;
    }

    if (cf.can_id & CAN_ERR_FLAG) {
        /* The only enabled errors should be bus-off and controller restart */
        if (cf.can_id & CAN_ERR_BUSOFF) {
            g_info("Bus-off condition detected");
            vc->config.status |= VIRTIO_CAN_S_CTRL_BUSOFF;
            vu_config_change_msg(dev);
        } else if (cf.can_id & CAN_ERR_RESTARTED) {
            g_info("Controller restarted, remove bus-off condition");
            vc->config.status &= ~VIRTIO_CAN_S_CTRL_BUSOFF;
            vu_config_change_msg(dev);
        }
    } else {
        VuVirtqElement *elem = NULL;
        virtio_can_rx *rx;
        uint32_t flags = 0;

        /* Receive data frames */
        elem = vu_queue_pop(dev, vq, sizeof(VuVirtqElement));
        if (!elem) {
            g_warning("No available queue element, frame will be lost");
            return;
        }

        assert(elem->in_num == 1);

        if (elem->in_sg[0].iov_len < sizeof(virtio_can_rx)) {
            vc_panic(dev, "Invalid virtio-can RX buffer");
            vu_queue_unpop(dev, vq, elem, 0);
            return;
        } else if (elem->in_sg[0].iov_len < sizeof(virtio_can_rx) + cf.len) {
            g_warning("RX buffer is not big enough, frame will be lost");
            vu_queue_unpop(dev, vq, elem, 0);
            return;
        }

        rx = (virtio_can_rx *)elem->in_sg[0].iov_base;

        rx->msg_type = cpu_to_le16(VIRTIO_CAN_RX);

        if (cf.can_id & CAN_EFF_FLAG) {
            rx->can_id = cpu_to_le32(cf.can_id & CAN_EFF_MASK);
            flags |= VIRTIO_CAN_FLAGS_EXTENDED;
        } else {
            rx->can_id = cpu_to_le32(cf.can_id & CAN_SFF_MASK);
        }

        if (cf.flags & CANFD_FDF) {
            flags |= VIRTIO_CAN_FLAGS_FD;
        } else if (cf.can_id & CAN_RTR_FLAG) {
            flags |= VIRTIO_CAN_FLAGS_RTR;
        }

        rx->flags = cpu_to_le32(flags);
        rx->length = cpu_to_le16(cf.len);
        memcpy(rx->sdu, cf.data, cf.len);

        if (cf.can_id & CAN_EFF_FLAG) {
            g_debug("RX %08X [%2u]", cf.can_id & CAN_EFF_MASK, cf.len);
        } else {
            g_debug("RX      %03X [%2u]", cf.can_id & CAN_SFF_MASK, cf.len);
        }

        vu_queue_push(dev, vq, elem, sizeof(virtio_can_rx) + cf.len);
        free(elem);

        vu_queue_notify(&vc->dev.parent, vq);
    }
}

static bool
vc_do_transmit_cc(VuCan *vc, uint32_t id, const uint8_t *data, uint16_t length,
                  bool extended, bool rtr)
{
    struct can_frame cf = { 0 };
    ssize_t bytes;

    if (extended) {
        cf.can_id = (id & CAN_EFF_MASK) | CAN_EFF_FLAG;
    } else {
        cf.can_id = (id & CAN_SFF_MASK);
    }

    cf.len = MIN(length, CAN_MAX_DLEN);
    if (rtr) {
        cf.can_id |= CAN_RTR_FLAG;
    }

    memcpy(cf.data, data, cf.len);

    bytes = write(vc->can_sock, &cf, sizeof(cf));
    if (bytes < 0) {
        g_warning("Failed to send CAN-CC frame: %s", g_strerror(errno));
        return false;
    }

    if (extended) {
        g_debug("TX %08X [%2u]", cf.can_id & CAN_EFF_MASK, cf.len);
    } else {
        g_debug("TX      %03X [%2u]", cf.can_id & CAN_SFF_MASK, cf.len);
    }

    return true;
}

static bool
vc_do_transmit_fd(VuCan *vc, uint32_t id, const uint8_t *data, uint16_t length,
                  bool fd, bool extended, bool rtr)
{
    struct canfd_frame cf = { 0 };
    ssize_t bytes;

    if (extended) {
        cf.can_id = (id & CAN_EFF_MASK) | CAN_EFF_FLAG;
    } else {
        cf.can_id = (id & CAN_SFF_MASK);
    }

    if (fd) {
        cf.flags = CANFD_FDF;
        cf.len = MIN(length, CANFD_MAX_DLEN);

        /* CHECKME: something is missing from virtio-can specs? */
        if (length > CAN_MAX_DLEN) {
            cf.flags |= CANFD_BRS;
        }
    } else {
        cf.len = MIN(length, CAN_MAX_DLEN);
        if (rtr) {
            cf.can_id |= CAN_RTR_FLAG;
        }
    }

    memcpy(cf.data, data, cf.len);

    bytes = write(vc->can_sock, &cf, sizeof(cf));
    if (bytes < 0) {
        g_warning("Failed to send CAN-%s frame: %s", fd ? "FD" : "CC",
                  g_strerror(errno));
        return false;
    }

    if (extended) {
        g_debug("TX %08X [%2u]", cf.can_id & CAN_EFF_MASK, cf.len);
    } else {
        g_debug("TX      %03X [%2u]", cf.can_id & CAN_SFF_MASK, cf.len);
    }

    return true;
}

static void
vc_handle_tx(VuDev *dev, int qidx)
{
    VuCan *vc = container_of(dev, VuCan, dev.parent);
    VuVirtq *vq = vu_get_queue(dev, qidx);

    for(;;) {
        VuVirtqElement *elem = NULL;
        virtio_can_tx_out *tx_req;
        virtio_can_tx_in *tx_rsp;
        uint16_t msg_type;
        uint32_t flags;
        uint32_t can_id;
        uint16_t length;
        bool tx_ok;

        elem = vu_queue_pop(dev, vq, sizeof(VuVirtqElement));
        if (!elem) {
            break;
        }

        assert(elem->out_num == 1 && elem->in_num == 1);

        if (elem->out_sg[0].iov_len < sizeof(virtio_can_tx_out)) {
            vc_panic(dev, "Invalid virtio-can TX request");
            break;
        }
        tx_req = (virtio_can_tx_out *)elem->out_sg[0].iov_base;

        if (elem->in_sg[0].iov_len < sizeof(virtio_can_tx_in)) {
            vc_panic(dev, "Invalid virtio-can TX response");
            break;
        }
        tx_rsp = (virtio_can_tx_in *)elem->in_sg[0].iov_base;

        msg_type = le16_to_cpu(tx_req->msg_type);
        flags = le32_to_cpu(tx_req->flags);
        can_id = le32_to_cpu(tx_req->can_id);
        length = le16_to_cpu(tx_req->length);

        if (elem->out_sg[0].iov_len < sizeof(virtio_can_tx_out) + length) {
            g_warning("Incomplete virtio-can TX request");
            tx_rsp->result = VIRTIO_CAN_RESULT_NOT_OK;
        } else {
            switch (msg_type) {
                case VIRTIO_CAN_TX:
                    if (vc->has_fd) {
                        tx_ok = vc_do_transmit_fd(vc, can_id, tx_req->sdu, length,
                                                  flags & VIRTIO_CAN_FLAGS_FD,
                                                  flags & VIRTIO_CAN_FLAGS_EXTENDED,
                                                  flags & VIRTIO_CAN_FLAGS_RTR);
                    } else {
                        tx_ok = vc_do_transmit_cc(vc, can_id, tx_req->sdu, length,
                                                  flags & VIRTIO_CAN_FLAGS_EXTENDED,
                                                  flags & VIRTIO_CAN_FLAGS_RTR);
                    }
                    if (tx_ok) {
                        tx_rsp->result = VIRTIO_CAN_RESULT_OK;
                    } else {
                        tx_rsp->result = VIRTIO_CAN_RESULT_NOT_OK;
                    }
                    break;
                default:
                    g_warning("Invalid msg_type 0x%04x\n", msg_type);
                    tx_rsp->result = VIRTIO_CAN_RESULT_NOT_OK;
                    break;
            }
        }

        vu_queue_push(dev, vq, elem, sizeof(virtio_can_tx_in));

        free(elem);
    }

    vu_queue_notify(&vc->dev.parent, vq);
}

static void
vc_handle_ctrl(VuDev *dev, int qidx)
{
    VuCan *vc = container_of(dev, VuCan, dev.parent);
    VuVirtq *vq = vu_get_queue(dev, qidx);

    for (;;) {
        VuVirtqElement *elem = NULL;
        virtio_can_control_out *ctrl_req;
        virtio_can_control_in *ctrl_rsp;
        uint16_t msg_type;

        elem = vu_queue_pop(dev, vq, sizeof(VuVirtqElement));
        if (!elem) {
            break;
        }

        assert(elem->out_num == 1 && elem->in_num == 1);

        if (elem->out_sg[0].iov_len < sizeof(virtio_can_control_out)) {
            vc_panic(dev, "Invalid virtio-can CTRL request");
            break;
        }
        ctrl_req = (virtio_can_control_out *)elem->out_sg[0].iov_base;

        if (elem->in_sg[0].iov_len < sizeof(virtio_can_control_in)) {
            vc_panic(dev, "Invalid virtio-can CTRL response");
            break;
        }
        ctrl_rsp = (virtio_can_control_in *)elem->in_sg[0].iov_base;

        msg_type = le16_to_cpu(ctrl_req->msg_type);

        switch (msg_type) {
            case VIRTIO_CAN_SET_CTRL_MODE_START:
                vc->started = true;
                g_info("Controller started");
                ctrl_rsp->result = VIRTIO_CAN_RESULT_OK;
                break;
            case VIRTIO_CAN_SET_CTRL_MODE_STOP:
                vc->started = false;
                g_info("Controller stopped");
                ctrl_rsp->result = VIRTIO_CAN_RESULT_OK;
                break;
            default:
                g_warning("Invalid CTRL msg_type 0x%04x\n", msg_type);
                ctrl_rsp->result = VIRTIO_CAN_RESULT_NOT_OK;
                break;
        }

        vu_queue_push(dev, vq, elem, sizeof(virtio_can_control_in));

        free(elem);
    }

    vu_queue_notify(&vc->dev.parent, vq);
}

static void
vc_queue_set_started(VuDev *dev, int qidx, bool started)
{
    VuCan *vc = container_of(dev, VuCan, dev.parent);
    VuVirtq *vq = vu_get_queue(dev, qidx);
    int q;

    g_debug("Queue %d %s", qidx, started ? "started" : "stopped");

    switch (qidx) {
    case VHOST_USER_CAN_TX_QUEUE:
        vu_set_queue_handler(dev, vq, started ? vc_handle_tx : NULL);
        break;
    case VHOST_USER_CAN_RX_QUEUE:
        vc->started &= started;
        break;
    case VHOST_USER_CAN_CTRL_QUEUE:
        vu_set_queue_handler(dev, vq, started ? vc_handle_ctrl : NULL);
        break;
    default:
        break;
    }

    for (q = 0; started && q < VHOST_USER_CAN_NUM_QUEUES; q++) {
        started &= vu_queue_started(dev, vu_get_queue(dev, q));
    }

    if (started && !vc->evsrc) {
        g_debug("All queues started, enable frame reception");
        vc->evsrc = vug_source_new(&vc->dev, vc->can_sock,
                                   G_IO_IN, vc_socketcan_receive, vc);
    }

    if (!started && vc->evsrc) {
        vug_source_destroy(vc->evsrc);
        vc->evsrc = NULL;
    }
}

static uint64_t
vc_get_features(VuDev *dev)
{
    VuCan *vc = container_of(dev, VuCan, dev.parent);
    uint64_t features = 0;

    features |= (1ULL << VIRTIO_CAN_F_CAN_CLASSIC);
    features |= (1ULL << VIRTIO_CAN_F_RTR_FRAMES);

    if (vc->has_fd) {
        features |= (1ULL << VIRTIO_CAN_F_CAN_FD);
    }

    return features;
}

static int
vc_get_config(VuDev *dev, uint8_t *config, uint32_t len)
{
    VuCan *vc = container_of(dev, VuCan, dev.parent);
    int ret = -1;

    if (len <= sizeof(vc->config)) {
        memcpy(config, &vc->config, len);
        ret = 0;
    }

    return ret;
}

static int
vc_set_config(VuDev *dev, const uint8_t *data,
              uint32_t offset, uint32_t size,
              uint32_t flags)
{
    /* Nothing to do */

    return 0;
}

static const VuDevIface vuiface = {
    .queue_set_started = vc_queue_set_started,
    .get_features = vc_get_features,
    .get_config = vc_get_config,
    .set_config = vc_set_config,
};

static int opt_fdnum = -1;
static char *opt_interface;
static char *opt_socket_path;
static gboolean opt_print_caps;

static GOptionEntry entries[] = {
    { "print-capabilities", 'c', 0, G_OPTION_ARG_NONE, &opt_print_caps,
      "Print capabilities", NULL },
    { "fd", 'f', 0, G_OPTION_ARG_INT, &opt_fdnum,
      "Use inherited fd socket", "FDNUM" },
    { "socket-path", 's', 0, G_OPTION_ARG_FILENAME, &opt_socket_path,
      "Use UNIX socket path", "PATH" },
    { "interface", 'i', 0, G_OPTION_ARG_STRING, &opt_interface,
      "CAN interface name", "CAN_IFACE" },
    { NULL, }
};

int
main(int argc, char *argv[])
{
    VuCan vc = { 0 };
    GOptionContext *context;
    struct sockaddr_can addr;
    struct ifreq ifr;
    int enable_canfd = 1;
    can_err_mask_t err_mask;
    GError *error = NULL;
    int fd;

    context = g_option_context_new(NULL);
    g_option_context_add_main_entries(context, entries, NULL);
    if (!g_option_context_parse(context, &argc, &argv, &error)) {
        g_printerr("Option parsing failed: %s\n", error->message);
        exit(EXIT_FAILURE);
    }
    if (opt_print_caps) {
        g_print("{\n");
        g_print("  \"type\": \"can\",\n");
        g_print("  \"features\": [\n");
        g_print("    \"interface\"\n");
        g_print("  ]\n");
        g_print("}\n");
        exit(EXIT_SUCCESS);
    }
    if (!opt_interface) {
        g_printerr("Please specify a CAN interface\n");
        exit(EXIT_FAILURE);
    }
    if ((!!opt_socket_path + (opt_fdnum != -1)) != 1) {
        g_printerr("Please specify either --fd or --socket-path\n");
        exit(EXIT_FAILURE);
    }

    /* Open socket */
    vc.can_sock = qemu_socket(PF_CAN, SOCK_RAW, CAN_RAW);
    if (vc.can_sock < 0) {
        g_printerr("Failed to create CAN_RAW socket: %s\n", g_strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Check if the CAN interface exists */
    memset(&ifr.ifr_name, 0, sizeof(ifr.ifr_name));
    strcpy(ifr.ifr_name, opt_interface);
    if (ioctl(vc.can_sock, SIOCGIFINDEX, &ifr) < 0) {
        g_printerr("CAN interface %s not available\n", opt_interface);
         exit(EXIT_FAILURE);
    }

    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    if (ioctl(vc.can_sock, SIOCGIFMTU, &ifr) < 0) {
        g_printerr("Failed to query MTU for interface %s: %s", opt_interface,
                   g_strerror(errno));
        exit(EXIT_FAILURE);
    }

    if (ifr.ifr_mtu >= CANFD_MTU) {
        /* interface is ok - try to switch the socket into CAN FD mode */
        if (setsockopt(vc.can_sock, SOL_CAN_RAW, CAN_RAW_FD_FRAMES,
                       &enable_canfd, sizeof(enable_canfd))) {
            g_printerr("Failed to enable CAN-FD mode");
            vc.has_fd = false;
        } else {
            vc.has_fd = true;
        }
    }

    /* Receive only bus-off error frames */
    err_mask = CAN_ERR_BUSOFF | CAN_ERR_RESTARTED;
    setsockopt(vc.can_sock, SOL_CAN_RAW, CAN_RAW_ERR_FILTER,
               &err_mask, sizeof(err_mask));

    if (bind(vc.can_sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        g_printerr("Failed to bind to host interface %s: %s", opt_interface,
                   g_strerror(errno));
        exit(EXIT_FAILURE);
    }

    memset(&vc.config, 0, sizeof(vc.config));

    if (opt_socket_path) {
        int lsock = unix_listen(opt_socket_path, &error_fatal);
        if (lsock < 0) {
            g_printerr("Failed to listen on %s", opt_socket_path);
            exit(EXIT_FAILURE);
        }
        fd = accept(lsock, NULL, NULL);
        close(lsock);
    } else {
        fd = opt_fdnum;
    }
    if (fd == -1) {
        g_printerr("Invalid vhost-user socket");
        exit(EXIT_FAILURE);
    }

    if (!vug_init(&vc.dev, VHOST_USER_CAN_NUM_QUEUES, fd, vc_panic, &vuiface)) {
        g_printerr("Failed to initialize libvhost-user-glib");
        exit(EXIT_FAILURE);
    }

    vc.loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(vc.loop);

    g_info("Exiting...");
    g_main_loop_unref(vc.loop);

    vug_deinit(&vc.dev);

    vug_source_destroy(vc.evsrc);

    return 0;
}
