/* spice-server spicevmc passthrough channel code

   Copyright (C) 2011 Red Hat, Inc.

   Red Hat Authors:
   Hans de Goede <hdegoede@redhat.com>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/
#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <errno.h>
#include <string.h>
#include <netinet/in.h> // IPPROTO_TCP
#include <netinet/tcp.h> // TCP_NODELAY

#include "common/generated_server_marshallers.h"

#include "char-device.h"
#include "red-channel.h"
#include "reds.h"
#include "migration-protocol.h"

/* todo: add flow control. i.e.,
 * (a) limit the tokens available for the client
 * (b) limit the tokens available for the server
 */
/* 64K should be enough for all but the largest writes + 32 bytes hdr */
#define BUF_SIZE (64 * 1024 + 32)

typedef struct SpiceVmcPipeItem {
    PipeItem base;

    /* writes which don't fit this will get split, this is not a problem */
    uint8_t buf[BUF_SIZE];
    uint32_t buf_used;
} SpiceVmcPipeItem;

typedef struct SpiceVmcState {
    RedChannel channel; /* Must be the first item */
    RedChannelClient *rcc;
    RedCharDevice *chardev;
    SpiceCharDeviceInstance *chardev_sin;
    SpiceVmcPipeItem *pipe_item;
    RedCharDeviceWriteBuffer *recv_from_client_buf;
    uint8_t port_opened;
} SpiceVmcState;

#define RED_TYPE_CHAR_DEVICE_SPICEVMC red_char_device_spicevmc_get_type()

#define RED_CHAR_DEVICE_SPICEVMC(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), RED_TYPE_CHAR_DEVICE_SPICEVMC, RedCharDeviceSpiceVmc))
#define RED_CHAR_DEVICE_SPICEVMC_CLASS(klass) (G_TYPE_CHECK_CLASS_CAST((klass), RED_TYPE_CHAR_DEVICE_SPICEVMC, RedCharDeviceSpiceVmcClass))
#define RED_IS_CHAR_DEVICE_SPICEVMC(obj) (G_TYPE_CHECK_INSTANCE_TYPE((obj), RED_TYPE_CHAR_DEVICE_SPICEVMC))
#define RED_IS_CHAR_DEVICE_SPICEVMC_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE((klass), RED_TYPE_CHAR_DEVICE_SPICEVMC))
#define RED_CHAR_DEVICE_SPICEVMC_GET_CLASS(obj) (G_TYPE_INSTANCE_GET_CLASS((obj), RED_TYPE_CHAR_DEVICE_SPICEVMC, RedCharDeviceSpiceVmcClass))

typedef struct RedCharDeviceSpiceVmc RedCharDeviceSpiceVmc;
typedef struct RedCharDeviceSpiceVmcClass RedCharDeviceSpiceVmcClass;

struct RedCharDeviceSpiceVmc {
    RedCharDevice parent;
};

struct RedCharDeviceSpiceVmcClass
{
    RedCharDeviceClass parent_class;
};

static GType red_char_device_spicevmc_get_type(void) G_GNUC_CONST;
static RedCharDevice *red_char_device_spicevmc_new(SpiceCharDeviceInstance *sin,
                                                   RedsState *reds,
                                                   void *opaque);

G_DEFINE_TYPE(RedCharDeviceSpiceVmc, red_char_device_spicevmc, RED_TYPE_CHAR_DEVICE)

typedef struct PortInitPipeItem {
    PipeItem base;
    char* name;
    uint8_t opened;
} PortInitPipeItem;

typedef struct PortEventPipeItem {
    PipeItem base;
    uint8_t event;
} PortEventPipeItem;

enum {
    PIPE_ITEM_TYPE_SPICEVMC_DATA = PIPE_ITEM_TYPE_CHANNEL_BASE,
    PIPE_ITEM_TYPE_SPICEVMC_MIGRATE_DATA,
    PIPE_ITEM_TYPE_PORT_INIT,
    PIPE_ITEM_TYPE_PORT_EVENT,
};

static PipeItem *spicevmc_chardev_read_msg_from_dev(SpiceCharDeviceInstance *sin,
                                                    void *opaque)
{
    SpiceVmcState *state = opaque;
    SpiceCharDeviceInterface *sif;
    SpiceVmcPipeItem *msg_item;
    int n;

    sif = spice_char_device_get_interface(sin);

    if (!state->rcc) {
        return NULL;
    }

    if (!state->pipe_item) {
        msg_item = spice_new0(SpiceVmcPipeItem, 1);
        pipe_item_init(&msg_item->base, PIPE_ITEM_TYPE_SPICEVMC_DATA);
    } else {
        spice_assert(state->pipe_item->buf_used == 0);
        msg_item = state->pipe_item;
        state->pipe_item = NULL;
    }

    n = sif->read(sin, msg_item->buf,
                  sizeof(msg_item->buf));
    if (n > 0) {
        spice_debug("read from dev %d", n);
        msg_item->buf_used = n;
        return (PipeItem *)msg_item;
    } else {
        state->pipe_item = msg_item;
        return NULL;
    }
}

static void spicevmc_chardev_send_msg_to_client(PipeItem *msg,
                                                RedClient *client,
                                                void *opaque)
{
    SpiceVmcState *state = opaque;
    SpiceVmcPipeItem *vmc_msg = (SpiceVmcPipeItem *)msg;

    spice_assert(state->rcc->client == client);
    pipe_item_ref(vmc_msg);
    red_channel_client_pipe_add_push(state->rcc, (PipeItem *)vmc_msg);
}

static void spicevmc_port_send_init(RedChannelClient *rcc)
{
    SpiceVmcState *state = SPICE_CONTAINEROF(rcc->channel, SpiceVmcState, channel);
    SpiceCharDeviceInstance *sin = state->chardev_sin;
    PortInitPipeItem *item = spice_malloc(sizeof(PortInitPipeItem));

    pipe_item_init(&item->base, PIPE_ITEM_TYPE_PORT_INIT);
    item->name = strdup(sin->portname);
    item->opened = state->port_opened;
    red_channel_client_pipe_add_push(rcc, &item->base);
}

static void spicevmc_port_send_event(RedChannelClient *rcc, uint8_t event)
{
    PortEventPipeItem *item = spice_malloc(sizeof(PortEventPipeItem));

    pipe_item_init(&item->base, PIPE_ITEM_TYPE_PORT_EVENT);
    item->event = event;
    red_channel_client_pipe_add_push(rcc, &item->base);
}

static void spicevmc_char_dev_send_tokens_to_client(RedClient *client,
                                                    uint32_t tokens,
                                                    void *opaque)
{
    spice_printerr("Not implemented!");
}

static void spicevmc_char_dev_remove_client(RedClient *client, void *opaque)
{
    SpiceVmcState *state = opaque;

    spice_printerr("vmc state %p, client %p", state, client);
    spice_assert(state->rcc && state->rcc->client == client);

    red_channel_client_shutdown(state->rcc);
}

static int spicevmc_red_channel_client_config_socket(RedChannelClient *rcc)
{
    int delay_val = 1;
    RedsStream *stream = red_channel_client_get_stream(rcc);

    if (rcc->channel->type == SPICE_CHANNEL_USBREDIR) {
        if (setsockopt(stream->socket, IPPROTO_TCP, TCP_NODELAY,
                &delay_val, sizeof(delay_val)) != 0) {
            if (errno != ENOTSUP && errno != ENOPROTOOPT) {
                spice_printerr("setsockopt failed, %s", strerror(errno));
                return FALSE;
            }
        }
    }

    return TRUE;
}

static void spicevmc_red_channel_client_on_disconnect(RedChannelClient *rcc)
{
    SpiceVmcState *state;
    SpiceCharDeviceInterface *sif;

    if (!rcc) {
        return;
    }

    state = SPICE_CONTAINEROF(rcc->channel, SpiceVmcState, channel);

    if (state->recv_from_client_buf) { /* partial message which wasn't pushed to device */
        red_char_device_write_buffer_release(state->chardev, state->recv_from_client_buf);
        state->recv_from_client_buf = NULL;
    }

    if (state->chardev) {
        if (red_char_device_client_exists(state->chardev, rcc->client)) {
            red_char_device_client_remove(state->chardev, rcc->client);
        } else {
            spice_printerr("client %p have already been removed from char dev %p",
                           rcc->client, state->chardev);
        }
    }

    /* Don't destroy the rcc if it is already being destroyed, as then
       red_client_destroy/red_channel_client_destroy will already do this! */
    if (!rcc->destroying)
        red_channel_client_destroy(rcc);

    state->rcc = NULL;
    sif = spice_char_device_get_interface(state->chardev_sin);
    if (sif->state) {
        sif->state(state->chardev_sin, 0);
    }
}

static SpiceVmcState *spicevmc_red_channel_client_get_state(RedChannelClient *rcc)
{
    return SPICE_CONTAINEROF(rcc->channel, SpiceVmcState, channel);
}

static int spicevmc_channel_client_handle_migrate_flush_mark(RedChannelClient *rcc)
{
    red_channel_client_pipe_add_type(rcc, PIPE_ITEM_TYPE_SPICEVMC_MIGRATE_DATA);
    return TRUE;
}

static int spicevmc_channel_client_handle_migrate_data(RedChannelClient *rcc,
                                                       uint32_t size, void *message)
{
    SpiceMigrateDataHeader *header;
    SpiceMigrateDataSpiceVmc *mig_data;
    SpiceVmcState *state;

    state = spicevmc_red_channel_client_get_state(rcc);

    header = (SpiceMigrateDataHeader *)message;
    mig_data = (SpiceMigrateDataSpiceVmc *)(header + 1);
    spice_assert(size >= sizeof(SpiceMigrateDataHeader) + sizeof(SpiceMigrateDataSpiceVmc));

    if (!migration_protocol_validate_header(header,
                                            SPICE_MIGRATE_DATA_SPICEVMC_MAGIC,
                                            SPICE_MIGRATE_DATA_SPICEVMC_VERSION)) {
        spice_error("bad header");
        return FALSE;
    }
    return red_char_device_restore(state->chardev, &mig_data->base);
}

static int spicevmc_red_channel_client_handle_message(RedChannelClient *rcc,
                                                      uint16_t type,
                                                      uint32_t size,
                                                      uint8_t *msg)
{
    SpiceVmcState *state;
    SpiceCharDeviceInterface *sif;

    state = spicevmc_red_channel_client_get_state(rcc);
    sif = spice_char_device_get_interface(state->chardev_sin);

    switch (type) {
    case SPICE_MSGC_SPICEVMC_DATA:
        spice_assert(state->recv_from_client_buf->buf == msg);
        state->recv_from_client_buf->buf_used = size;
        red_char_device_write_buffer_add(state->chardev, state->recv_from_client_buf);
        state->recv_from_client_buf = NULL;
        break;
    case SPICE_MSGC_PORT_EVENT:
        if (size != sizeof(uint8_t)) {
            spice_warning("bad port event message size");
            return FALSE;
        }
        if (sif->base.minor_version >= 2 && sif->event != NULL)
            sif->event(state->chardev_sin, *msg);
        break;
    default:
        return red_channel_client_handle_message(rcc, size, type, msg);
    }

    return TRUE;
}

static uint8_t *spicevmc_red_channel_alloc_msg_rcv_buf(RedChannelClient *rcc,
                                                       uint16_t type,
                                                       uint32_t size)
{
    SpiceVmcState *state;

    state = SPICE_CONTAINEROF(rcc->channel, SpiceVmcState, channel);

    switch (type) {
    case SPICE_MSGC_SPICEVMC_DATA:
        assert(!state->recv_from_client_buf);

        state->recv_from_client_buf = red_char_device_write_buffer_get(state->chardev,
                                                                       rcc->client,
                                                                       size);
        if (!state->recv_from_client_buf) {
            spice_error("failed to allocate write buffer");
            return NULL;
        }
        return state->recv_from_client_buf->buf;

    default:
        return spice_malloc(size);
    }

}

static void spicevmc_red_channel_release_msg_rcv_buf(RedChannelClient *rcc,
                                                     uint16_t type,
                                                     uint32_t size,
                                                     uint8_t *msg)
{
    SpiceVmcState *state;

    state = SPICE_CONTAINEROF(rcc->channel, SpiceVmcState, channel);

    switch (type) {
    case SPICE_MSGC_SPICEVMC_DATA:
        if (state->recv_from_client_buf) { /* buffer wasn't pushed to device */
            red_char_device_write_buffer_release(state->chardev, state->recv_from_client_buf);
            state->recv_from_client_buf = NULL;
        }
        break;
    default:
        free(msg);
    }
}

static void spicevmc_red_channel_hold_pipe_item(RedChannelClient *rcc,
    PipeItem *item)
{
    /* NOOP */
}

static void spicevmc_red_channel_send_data(RedChannelClient *rcc,
                                           SpiceMarshaller *m,
                                           PipeItem *item)
{
    SpiceVmcPipeItem *i = SPICE_CONTAINEROF(item, SpiceVmcPipeItem, base);

    red_channel_client_init_send_data(rcc, SPICE_MSG_SPICEVMC_DATA, item);
    spice_marshaller_add_ref(m, i->buf, i->buf_used);
}

static void spicevmc_red_channel_send_migrate_data(RedChannelClient *rcc,
                                                   SpiceMarshaller *m,
                                                   PipeItem *item)
{
    SpiceVmcState *state;

    state = SPICE_CONTAINEROF(rcc->channel, SpiceVmcState, channel);
    red_channel_client_init_send_data(rcc, SPICE_MSG_MIGRATE_DATA, item);
    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_SPICEVMC_MAGIC);
    spice_marshaller_add_uint32(m, SPICE_MIGRATE_DATA_SPICEVMC_VERSION);

    red_char_device_migrate_data_marshall(state->chardev, m);
}

static void spicevmc_red_channel_send_port_init(RedChannelClient *rcc,
                                                SpiceMarshaller *m,
                                                PipeItem *item)
{
    PortInitPipeItem *i = SPICE_CONTAINEROF(item, PortInitPipeItem, base);
    SpiceMsgPortInit init;

    red_channel_client_init_send_data(rcc, SPICE_MSG_PORT_INIT, item);
    init.name = (uint8_t *)i->name;
    init.name_size = strlen(i->name) + 1;
    init.opened = i->opened;
    spice_marshall_msg_port_init(m, &init);
}

static void spicevmc_red_channel_send_port_event(RedChannelClient *rcc,
                                                 SpiceMarshaller *m,
                                                 PipeItem *item)
{
    PortEventPipeItem *i = SPICE_CONTAINEROF(item, PortEventPipeItem, base);
    SpiceMsgPortEvent event;

    red_channel_client_init_send_data(rcc, SPICE_MSG_PORT_EVENT, item);
    event.event = i->event;
    spice_marshall_msg_port_event(m, &event);
}

static void spicevmc_red_channel_send_item(RedChannelClient *rcc,
                                           PipeItem *item)
{
    SpiceMarshaller *m = red_channel_client_get_marshaller(rcc);

    switch (item->type) {
    case PIPE_ITEM_TYPE_SPICEVMC_DATA:
        spicevmc_red_channel_send_data(rcc, m, item);
        break;
    case PIPE_ITEM_TYPE_SPICEVMC_MIGRATE_DATA:
        spicevmc_red_channel_send_migrate_data(rcc, m, item);
        break;
    case PIPE_ITEM_TYPE_PORT_INIT:
        spicevmc_red_channel_send_port_init(rcc, m, item);
        break;
    case PIPE_ITEM_TYPE_PORT_EVENT:
        spicevmc_red_channel_send_port_event(rcc, m, item);
        break;
    default:
        spice_error("bad pipe item %d", item->type);
        free(item);
        return;
    }
    red_channel_client_begin_send_message(rcc);
}

static void spicevmc_red_channel_release_pipe_item(RedChannelClient *rcc,
    PipeItem *item, int item_pushed)
{
    if (item->type == PIPE_ITEM_TYPE_SPICEVMC_DATA) {
        pipe_item_unref(item);
    } else {
        free(item);
    }
}

static void spicevmc_connect(RedChannel *channel, RedClient *client,
    RedsStream *stream, int migration, int num_common_caps,
    uint32_t *common_caps, int num_caps, uint32_t *caps)
{
    RedChannelClient *rcc;
    SpiceVmcState *state;
    SpiceCharDeviceInstance *sin;
    SpiceCharDeviceInterface *sif;

    state = SPICE_CONTAINEROF(channel, SpiceVmcState, channel);
    sin = state->chardev_sin;

    if (state->rcc) {
        spice_printerr("channel client %d:%d (%p) already connected, refusing second connection",
                       channel->type, channel->id, state->rcc);
        // TODO: notify client in advance about the in use channel using
        // SPICE_MSG_MAIN_CHANNEL_IN_USE (for example)
        reds_stream_free(stream);
        return;
    }

    rcc = red_channel_client_create(sizeof(RedChannelClient), channel, client, stream,
                                    FALSE,
                                    num_common_caps, common_caps,
                                    num_caps, caps);
    if (!rcc) {
        return;
    }
    state->rcc = rcc;
    red_channel_client_ack_zero_messages_window(rcc);

    if (strcmp(sin->subtype, "port") == 0) {
        spicevmc_port_send_init(rcc);
    }

    if (!red_char_device_client_add(state->chardev, client, FALSE, 0, ~0, ~0,
                                    red_channel_client_is_waiting_for_migrate_data(rcc))) {
        spice_warning("failed to add client to spicevmc");
        red_channel_client_disconnect(rcc);
        return;
    }

    sif = spice_char_device_get_interface(state->chardev_sin);
    if (sif->state) {
        sif->state(sin, 1);
    }
}

RedCharDevice *spicevmc_device_connect(RedsState *reds,
                                       SpiceCharDeviceInstance *sin,
                                       uint8_t channel_type)
{
    static uint8_t id[256] = { 0, };
    SpiceVmcState *state;
    ChannelCbs channel_cbs = { NULL, };
    ClientCbs client_cbs = { NULL, };

    channel_cbs.config_socket = spicevmc_red_channel_client_config_socket;
    channel_cbs.on_disconnect = spicevmc_red_channel_client_on_disconnect;
    channel_cbs.send_item = spicevmc_red_channel_send_item;
    channel_cbs.hold_item = spicevmc_red_channel_hold_pipe_item;
    channel_cbs.release_item = spicevmc_red_channel_release_pipe_item;
    channel_cbs.alloc_recv_buf = spicevmc_red_channel_alloc_msg_rcv_buf;
    channel_cbs.release_recv_buf = spicevmc_red_channel_release_msg_rcv_buf;
    channel_cbs.handle_migrate_flush_mark = spicevmc_channel_client_handle_migrate_flush_mark;
    channel_cbs.handle_migrate_data = spicevmc_channel_client_handle_migrate_data;

    state = (SpiceVmcState*)red_channel_create(sizeof(SpiceVmcState), reds,
                                   reds_get_core_interface(reds), channel_type, id[channel_type]++,
                                   FALSE /* handle_acks */,
                                   spicevmc_red_channel_client_handle_message,
                                   &channel_cbs,
                                   SPICE_MIGRATE_NEED_FLUSH | SPICE_MIGRATE_NEED_DATA_TRANSFER);
    red_channel_init_outgoing_messages_window(&state->channel);

    client_cbs.connect = spicevmc_connect;
    red_channel_register_client_cbs(&state->channel, &client_cbs, NULL);

    state->chardev = red_char_device_spicevmc_new(sin, reds, state);
    state->chardev_sin = sin;

    reds_register_channel(reds, &state->channel);
    return state->chardev;
}

/* Must be called from RedClient handling thread. */
void spicevmc_device_disconnect(RedsState *reds, SpiceCharDeviceInstance *sin)
{
    SpiceVmcState *state;

    /* FIXME */
    state = (SpiceVmcState *)red_char_device_opaque_get((RedCharDevice*)sin->st);

    if (state->recv_from_client_buf) {
        red_char_device_write_buffer_release(state->chardev, state->recv_from_client_buf);
    }
    /* FIXME */
    red_char_device_destroy((RedCharDevice*)sin->st);
    state->chardev = NULL;
    sin->st = NULL;

    reds_unregister_channel(reds, &state->channel);
    free(state->pipe_item);
    red_channel_destroy(&state->channel);
}

SPICE_GNUC_VISIBLE void spice_server_port_event(SpiceCharDeviceInstance *sin, uint8_t event)
{
    SpiceVmcState *state;

    if (sin->st == NULL) {
        spice_warning("no SpiceCharDeviceState attached to instance %p", sin);
        return;
    }

    /* FIXME */
    state = (SpiceVmcState *)red_char_device_opaque_get((RedCharDevice*)sin->st);
    if (event == SPICE_PORT_EVENT_OPENED) {
        state->port_opened = TRUE;
    } else if (event == SPICE_PORT_EVENT_CLOSED) {
        state->port_opened = FALSE;
    }

    if (state->rcc == NULL) {
        return;
    }

    spicevmc_port_send_event(state->rcc, event);
}

static void
red_char_device_spicevmc_class_init(RedCharDeviceSpiceVmcClass *klass)
{
    RedCharDeviceClass *char_dev_class = RED_CHAR_DEVICE_CLASS(klass);

    char_dev_class->read_one_msg_from_device = spicevmc_chardev_read_msg_from_dev;
    char_dev_class->send_msg_to_client = spicevmc_chardev_send_msg_to_client;
    char_dev_class->send_tokens_to_client = spicevmc_char_dev_send_tokens_to_client;
    char_dev_class->remove_client = spicevmc_char_dev_remove_client;
}

static void
red_char_device_spicevmc_init(RedCharDeviceSpiceVmc *self)
{
}

static RedCharDevice *
red_char_device_spicevmc_new(SpiceCharDeviceInstance *sin,
                             RedsState *reds,
                             void *opaque)
{
    return g_object_new(RED_TYPE_CHAR_DEVICE_SPICEVMC,
                        "sin", sin,
                        "spice-server", reds,
                        "client-tokens-interval", 0ULL,
                        "self-tokens", ~0ULL,
                        "opaque", opaque,
                        NULL);
}
