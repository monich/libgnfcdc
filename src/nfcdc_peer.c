/*
 * Copyright (C) 2021 Jolla Ltd.
 * Copyright (C) 2021 Slava Monich <slava@monich.com>
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *   1. Redistributions of source code must retain the above copyright
 *      notice, this list of conditions and the following disclaimer.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice, this list of conditions and the following disclaimer
 *      in the documentation and/or other materials provided with the
 *      distribution.
 *   3. Neither the names of the copyright holders nor the names of its
 *      contributors may be used to endorse or promote products derived
 *      from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation
 * are those of the authors and should not be interpreted as representing
 * any official policies, either expressed or implied.
 */

#include "nfcdc_adapter_p.h"
#include "nfcdc_peer.h"
#include "nfcdc_base.h"
#include "nfcdc_dbus.h"
#include "nfcdc_log.h"

#include <gutil_macros.h>
#include <gutil_misc.h>
#include <gutil_strv.h>

#include "org.sailfishos.nfc.Peer.h"

#include <gio/gunixfdlist.h>

enum nfc_peer_client_adapter_signals {
    ADAPTER_VALID_CHANGED,
    ADAPTER_PEERS_CHANGED,
    ADAPTER_SIGNAL_COUNT
};

typedef NfcClientBaseClass NfcPeerClientObjectClass;
typedef struct nfc_peer_client_object {
    NfcClientBase base;
    NfcPeerClient pub;
    GDBusConnection* connection;
    NfcAdapterClient* adapter;
    gulong adapter_event_id[ADAPTER_SIGNAL_COUNT];
    OrgSailfishosNfcPeer* proxy;
    gboolean proxy_initializing;
} NfcPeerClientObject;

G_DEFINE_TYPE(NfcPeerClientObject, nfc_peer_client_object, \
    NFC_CLIENT_TYPE_BASE)
#define PARENT_CLASS nfc_peer_client_object_parent_class
#define THIS_TYPE nfc_peer_client_object_get_type()
#define THIS(obj) G_TYPE_CHECK_INSTANCE_CAST((obj), THIS_TYPE, \
    NfcPeerClientObject)

NFC_CLIENT_BASE_ASSERT_COUNT(NFC_PEER_PROPERTY_VALID);
NFC_CLIENT_BASE_ASSERT_COUNT(NFC_PEER_PROPERTY_COUNT);

#define SIGNAL_BIT_(x) NFC_CLIENT_BASE_SIGNAL_BIT(NFC_PEER_PROPERTY_##x)

#define nfc_peer_client_emit_queued_signals(self) \
    nfc_client_base_emit_queued_signals(&(self)->base)
#define nfc_peer_client_queue_signal(self,NAME) \
    ((self)->base.queued_signals |= SIGNAL_BIT_(NAME))

typedef struct nfc_peer_client_connect_data {
    NfcPeerClient* peer;
    NfcPeerClientConnectionFunc callback;
    void* user_data;
    GDestroyNotify destroy;
    gboolean (*finish_call) (
        OrgSailfishosNfcPeer* proxy,
        GVariant** fd,
        GUnixFDList** fdl,
        GAsyncResult* result,
        GError** error);
} NfcPeerClientConnectData;

static GHashTable* nfc_peer_client_table;

static
void
nfc_peer_client_reinit(
    NfcPeerClientObject* self);

/*==========================================================================*
 * Implementation
 *==========================================================================*/

static inline
NfcPeerClientObject*
nfc_peer_client_object_cast(
    NfcPeerClient* peer)
{
    return peer ? THIS(G_CAST(peer, NfcPeerClientObject, pub)) : NULL;
}

static
void
nfc_peer_client_update_valid_and_present(
    NfcPeerClientObject* self)
{
    NfcPeerClient* peer = &self->pub;
    NfcAdapterClient* adapter = self->adapter;
    gboolean valid, present;

    if (!adapter->valid || self->proxy_initializing) {
        valid = FALSE;
        present = FALSE;
    } else {
        valid = TRUE;
        present = (self->proxy && adapter->present &&
            gutil_strv_contains(adapter->peers, peer->path));
    }
    if (peer->valid != valid) {
        peer->valid = valid;
        nfc_peer_client_queue_signal(self, VALID);
    }
    if (peer->present != present) {
        peer->present = present;
        nfc_peer_client_queue_signal(self, PRESENT);
    }
}

static
void
nfc_peer_client_drop_proxy(
    NfcPeerClientObject* self)
{
    NfcPeerClient* peer = &self->pub;

    GASSERT(!self->proxy_initializing);
    if (self->proxy) {
        g_object_unref(self->proxy);
        self->proxy = NULL;
    }
    if (peer->valid) {
        peer->valid = FALSE;
        nfc_peer_client_queue_signal(self, VALID);
    }
    if (peer->present) {
        peer->present = FALSE;
        nfc_peer_client_queue_signal(self, PRESENT);
    }
    if (peer->wks) {
        peer->wks = 0;
        nfc_peer_client_queue_signal(self, WKS);
    }
}

static
void
nfc_peer_client_update(
    NfcPeerClientObject* self)
{
    NfcPeerClient* pub = &self->pub;

    if (gutil_strv_contains(self->adapter->peers, pub->path)) {
        if (!self->proxy && !self->proxy_initializing) {
            nfc_peer_client_reinit(self);
        }
    } else if (!self->proxy_initializing) {
        nfc_peer_client_drop_proxy(self);
    }
    nfc_peer_client_update_valid_and_present(self);
}

static
void
nfc_peer_client_adapter_changed(
    NfcAdapterClient* adapter,
    NFC_ADAPTER_PROPERTY property,
    void* user_data)
{
    NfcPeerClientObject* self = THIS(user_data);

    nfc_peer_client_update(self);
    nfc_peer_client_emit_queued_signals(self);
}

static
void
nfc_peer_client_get_all_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcPeerClientObject* self = THIS(user_data);
    NfcPeerClient* peer = &self->pub;
    GError* error = NULL;
    gboolean present;
    guint wks;

    GASSERT(self->proxy_initializing);
    self->proxy_initializing = FALSE;
    if (org_sailfishos_nfc_peer_call_get_all_finish(self->proxy, NULL,
        &present, NULL, NULL, &wks, result, &error)) {
        GVERBOSE_("%s", peer->path);
        if (peer->wks != wks) {
            peer->wks = wks;
            nfc_peer_client_queue_signal(self, WKS);
        }
    } else {
        GERR("%s", GERRMSG(error));
        g_error_free(error);
        /* Need to retry? */
        nfc_peer_client_drop_proxy(self);
    }
    nfc_peer_client_update_valid_and_present(self);
    nfc_peer_client_emit_queued_signals(self);
    g_object_unref(self);
}

static
void
nfc_peer_client_init_proxy_done(
    GObject* connection,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcPeerClientObject* self = THIS(user_data);
    GError* error = NULL;

    GASSERT(!self->proxy);
    GASSERT(self->proxy_initializing);
    self->proxy = org_sailfishos_nfc_peer_proxy_new_finish(result, &error);
    if (self->proxy) {
        org_sailfishos_nfc_peer_call_get_all(self->proxy, NULL,
            nfc_peer_client_get_all_done, g_object_ref(self));
    } else {
        GERR("%s", GERRMSG(error));
        g_error_free(error);
        self->proxy_initializing = FALSE;
        nfc_peer_client_update_valid_and_present(self);
        nfc_peer_client_emit_queued_signals(self);
    }
    g_object_unref(self);
}

static
void
nfc_peer_client_init_start(
    NfcPeerClientObject* self)
{
    org_sailfishos_nfc_peer_proxy_new(self->connection,
        G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, NFCD_DBUS_DAEMON_NAME,
        self->pub.path, NULL, nfc_peer_client_init_proxy_done,
        g_object_ref(self));
}

static
void
nfc_peer_client_reinit(
    NfcPeerClientObject* self)
{
    GASSERT(!self->proxy_initializing);
    self->proxy_initializing = TRUE;
    nfc_peer_client_init_start(self);
}

static
void
nfc_peer_client_connect_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcPeerClientConnectData* data = user_data;
    GVariant* fd = NULL;
    GUnixFDList* fdl = NULL;
    GError* error = NULL;

    if (data->finish_call(ORG_SAILFISHOS_NFC_PEER(proxy), &fd, &fdl,
        result, &error)) {
        if (data->callback) {
            data->callback(data->peer, g_unix_fd_list_peek_fds(fdl, NULL)[0],
                NULL, data->user_data);
        }
        g_variant_unref(fd);
        g_object_unref(fdl);
    } else {
        if (error->code == G_IO_ERROR_CANCELLED) {
            GDEBUG("Connection cancelled");
        } else {
            GERR("%s", GERRMSG(error));
            if (data->callback) {
                data->callback(data->peer, -1, error, data->user_data);
            }
        }
        g_error_free(error);
    }
    if (data->destroy) {
        data->destroy(data->user_data);
    }
    nfc_peer_client_unref(data->peer);
    gutil_slice_free(data);
}

/*==========================================================================*
 * API
 *==========================================================================*/

NfcPeerClient*
nfc_peer_client_new(
    const char* path)
{
    if (G_LIKELY(path) && g_variant_is_object_path(path)) {
        const char* sep = strrchr(path, '/');

        if (sep > path) {
            char* adapter_path = g_strndup(path, sep - path);
            NfcPeerClientObject* self = NULL;

            if (nfc_peer_client_table) {
                self = g_hash_table_lookup(nfc_peer_client_table, path);
            }
            if (self) {
                g_object_ref(self);
            } else {
                char* key = g_strdup(path);

                GVERBOSE_("%s", path);
                self = g_object_new(THIS_TYPE, NULL);
                if (!nfc_peer_client_table) {
                    nfc_peer_client_table = g_hash_table_new_full(g_str_hash,
                        g_str_equal, g_free, NULL);
                }
                g_hash_table_insert(nfc_peer_client_table, key, self);
                self->pub.path = key;
                self->adapter = nfc_adapter_client_new(adapter_path);
                self->adapter_event_id[ADAPTER_VALID_CHANGED] =
                    nfc_adapter_client_add_property_handler(self->adapter,
                        NFC_ADAPTER_PROPERTY_VALID,
                        nfc_peer_client_adapter_changed, self);
                self->adapter_event_id[ADAPTER_PEERS_CHANGED] =
                    nfc_adapter_client_add_property_handler(self->adapter,
                        NFC_ADAPTER_PROPERTY_PEERS,
                        nfc_peer_client_adapter_changed, self);
                self->connection = nfc_adapter_client_connection(self->adapter);
                nfc_peer_client_update(self);
                if (self->connection) {
                    g_object_ref(self->connection);
                    nfc_peer_client_init_start(self);
                }
            }
            g_free(adapter_path);
            return &self->pub;
        }
    }
    return NULL;
}

NfcPeerClient*
nfc_peer_client_ref(
    NfcPeerClient* peer)
{
    if (G_LIKELY(peer)) {
        g_object_ref(nfc_peer_client_object_cast(peer));
        return peer;
    } else {
        return NULL;
    }
}

void
nfc_peer_client_unref(
    NfcPeerClient* peer)
{
    if (G_LIKELY(peer)) {
        g_object_unref(nfc_peer_client_object_cast(peer));
    }
}

gboolean
nfc_peer_client_connect_sap(
    NfcPeerClient* peer,
    guint rsap,
    GCancellable* cancel,
    NfcPeerClientConnectionFunc callback,
    void* user_data,
    GDestroyNotify destroy)
{
    NfcPeerClientObject* self = nfc_peer_client_object_cast(peer);

    if (G_LIKELY(self) && G_LIKELY(rsap) && G_LIKELY(self->proxy)) {
        NfcPeerClientConnectData* data = NULL;
        GAsyncReadyCallback complete = NULL;

        if (callback || destroy) {
            complete = nfc_peer_client_connect_done;
            data = g_slice_new(NfcPeerClientConnectData);
            data->peer = nfc_peer_client_ref(peer);
            data->callback = callback;
            data->user_data = user_data;
            data->destroy = destroy;
            data->finish_call =
                org_sailfishos_nfc_peer_call_connect_access_point_finish;
        }

        org_sailfishos_nfc_peer_call_connect_access_point(self->proxy, rsap,
            NULL, cancel, complete, data);
        return TRUE;
    }
    return FALSE;
}

gboolean
nfc_peer_client_connect_sn(
    NfcPeerClient* peer,
    const char* sn,
    GCancellable* cancel,
    NfcPeerClientConnectionFunc callback,
    void* user_data,
    GDestroyNotify destroy)
{
    NfcPeerClientObject* self = nfc_peer_client_object_cast(peer);

    if (G_LIKELY(self) && G_LIKELY(sn) && G_LIKELY(self->proxy)) {
        NfcPeerClientConnectData* data = NULL;
        GAsyncReadyCallback complete = NULL;

        if (callback || destroy) {
            complete = nfc_peer_client_connect_done;
            data = g_slice_new(NfcPeerClientConnectData);
            data->peer = nfc_peer_client_ref(peer);
            data->callback = callback;
            data->user_data = user_data;
            data->destroy = destroy;
            data->finish_call =
                org_sailfishos_nfc_peer_call_connect_service_name_finish;
        }

        org_sailfishos_nfc_peer_call_connect_service_name(self->proxy, sn,
            NULL, cancel, complete, data);
        return TRUE;
    }
    return FALSE;
}

gulong
nfc_peer_client_add_property_handler(
    NfcPeerClient* peer,
    NFC_PEER_PROPERTY property,
    NfcPeerPropertyFunc callback,
    void* user_data)
{
    NfcPeerClientObject* self = nfc_peer_client_object_cast(peer);

    return G_LIKELY(self) ? nfc_client_base_add_property_handler(&self->base,
        property, (NfcClientBasePropertyFunc) callback, user_data) : 0;
}

void
nfc_peer_client_remove_handler(
    NfcPeerClient* peer,
    gulong id)
{
    if (G_LIKELY(id)) {
        NfcPeerClientObject* self = nfc_peer_client_object_cast(peer);

        if (G_LIKELY(self)) {
            g_signal_handler_disconnect(self, id);
        }
    }
}

void
nfc_peer_client_remove_handlers(
    NfcPeerClient* peer,
    gulong* ids,
    guint n)
{
    gutil_disconnect_handlers(nfc_peer_client_object_cast(peer), ids, n);
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
nfc_peer_client_object_init(
    NfcPeerClientObject* self)
{
    self->proxy_initializing = TRUE;
}

static
void
nfc_peer_client_object_finalize(
    GObject* object)
{
    NfcPeerClientObject* self = THIS(object);
    NfcPeerClient* peer = &self->pub;

    GVERBOSE_("%s", peer->path);
    nfc_peer_client_drop_proxy(self);
    nfc_adapter_client_remove_all_handlers(self->adapter,
        self->adapter_event_id);
    nfc_adapter_client_unref(self->adapter);
    if (self->connection) {
        g_object_unref(self->connection);
    }
    g_hash_table_remove(nfc_peer_client_table, peer->path);
    if (g_hash_table_size(nfc_peer_client_table) == 0) {
        g_hash_table_unref(nfc_peer_client_table);
        nfc_peer_client_table = NULL;
    }
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
nfc_peer_client_object_class_init(
    NfcPeerClientObjectClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = nfc_peer_client_object_finalize;
    klass->public_offset = G_STRUCT_OFFSET(NfcPeerClientObject, pub);
    klass->valid_offset = G_STRUCT_OFFSET(NfcPeerClientObject, pub.valid);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
