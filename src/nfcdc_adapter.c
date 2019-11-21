/*
 * Copyright (C) 2019 Jolla Ltd.
 * Copyright (C) 2019 Slava Monich <slava@monich.com>
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
 *      notice, this list of conditions and the following disclaimer in
 *      the documentation and/or other materials provided with the
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
#include "nfcdc_daemon_p.h"
#include "nfcdc_dbus.h"
#include "nfcdc_base.h"
#include "nfcdc_log.h"

#include <gutil_macros.h>
#include <gutil_misc.h>
#include <gutil_strv.h>

#include "org.sailfishos.nfc.Adapter.h"

enum nfc_adapter_daemon_signals {
    DAEMON_VALID_CHANGED,
    DAEMON_ADAPTERS_CHANGED,
    DAEMON_SIGNAL_COUNT
};

enum nfc_adapter_client_proxy_signals {
    PROXY_ENABLED_CHANGED,
    PROXY_POWERED_CHANGED,
    PROXY_MODE_CHANGED,
    PROXY_TARGET_PRESENT_CHANGED,
    PROXY_TAGS_CHANGED,
    PROXY_SIGNAL_COUNT
};

typedef NfcClientBaseClass NfcAdapterClientObjectClass;
typedef struct nfc_adapter_client_object {
    NfcClientBase base;
    NfcAdapterClient pub;
    NfcDaemonClient* daemon;
    gulong daemon_event_id[DAEMON_SIGNAL_COUNT];
    const char* name;
    GStrV* tags;
    GDBusConnection* connection;
    OrgSailfishosNfcAdapter* proxy;
    gulong proxy_signal_id[PROXY_SIGNAL_COUNT];
    gboolean proxy_initializing;
} NfcAdapterClientObject;

G_DEFINE_TYPE(NfcAdapterClientObject, nfc_adapter_client_object, \
        NFC_CLIENT_TYPE_BASE)
#define NFC_CLIENT_TYPE_ADAPTER (nfc_adapter_client_object_get_type())
#define NFC_ADAPTER_CLIENT_OBJECT(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),\
	NFC_CLIENT_TYPE_ADAPTER, NfcAdapterClientObject))

NFC_CLIENT_BASE_ASSERT_VALID(NFC_ADAPTER_PROPERTY_VALID);
NFC_CLIENT_BASE_ASSERT_COUNT(NFC_ADAPTER_PROPERTY_COUNT);

#define SIGNAL_BIT_(x) NFC_CLIENT_BASE_SIGNAL_BIT(NFC_ADAPTER_PROPERTY_##x)

#define nfc_adapter_client_emit_queued_signals(self) \
    nfc_client_base_emit_queued_signals(&(self)->base)
#define nfc_adapter_client_queue_signal(self,NAME) \
    ((self)->base.queued_signals |= SIGNAL_BIT_(NAME))

static char* nfc_adapter_client_empty_strv = NULL;
static GHashTable* nfc_adapter_client_table;

static
void
nfc_adapter_client_reinit(
    NfcAdapterClientObject* self);

/*==========================================================================*
 * Implementation
 *==========================================================================*/

static inline
NfcAdapterClientObject*
nfc_adapter_client_object_cast(
    NfcAdapterClient* pub)
{
    return G_LIKELY(pub) ?
        NFC_ADAPTER_CLIENT_OBJECT(G_CAST(pub, NfcAdapterClientObject, pub)) :
        NULL;
}

static
void
nfc_adapter_client_update_valid_and_present(
    NfcAdapterClientObject* self)
{
    NfcAdapterClient* pub = &self->pub;
    NfcDaemonClient* daemon = self->daemon;
    gboolean valid, present;

    if (!daemon->valid || self->proxy_initializing) {
        valid = FALSE;
        present = FALSE;
    } else {
        valid = TRUE;
        present = (self->proxy && daemon->present &&
            gutil_strv_contains(daemon->adapters, pub->path));
    }
    if (pub->valid != valid) {
        pub->valid = valid;
        nfc_adapter_client_queue_signal(self, VALID);
    }
    if (pub->present != present) {
        pub->present = present;
        nfc_adapter_client_queue_signal(self, PRESENT);
    }
}

static
void
nfc_adapter_client_drop_proxy(
    NfcAdapterClientObject* self)
{
    NfcAdapterClient* pub = &self->pub;

    GASSERT(!self->proxy_initializing);
    if (self->proxy) {
        gutil_disconnect_handlers(self->proxy, self->proxy_signal_id,
            G_N_ELEMENTS(self->proxy_signal_id));
        g_object_unref(self->proxy);
        self->proxy = NULL;
    }
    if (pub->valid) {
        pub->valid = FALSE;
        nfc_adapter_client_queue_signal(self, VALID);
    }
    if (pub->present) {
        pub->present = FALSE;
        nfc_adapter_client_queue_signal(self, PRESENT);
    }
    if (pub->enabled) {
        pub->enabled = FALSE;
        nfc_adapter_client_queue_signal(self, ENABLED);
    }
    if (pub->powered) {
        pub->powered = FALSE;
        nfc_adapter_client_queue_signal(self, POWERED);
    }
    if (pub->mode) {
        pub->mode = NFC_ADAPTER_MODE_NONE;
        nfc_adapter_client_queue_signal(self, POWERED);
    }
    if (pub->target_present) {
        pub->target_present = FALSE;
        nfc_adapter_client_queue_signal(self, TARGET_PRESENT);
    }
    if (pub->tags[0]) {
        g_strfreev(self->tags);
        self->tags = NULL;
        pub->tags = &nfc_adapter_client_empty_strv;
        nfc_adapter_client_queue_signal(self, TAGS);
    }
}

static
void
nfc_adapter_client_update(
    NfcAdapterClientObject* self)
{
    NfcAdapterClient* pub = &self->pub;

    if (gutil_strv_contains(self->daemon->adapters, pub->path)) {
        if (!self->proxy && !self->proxy_initializing) {
            nfc_adapter_client_reinit(self);
        }
    } else if (!self->proxy_initializing) {
        nfc_adapter_client_drop_proxy(self);
    }
    nfc_adapter_client_update_valid_and_present(self);
}

static
void
nfc_adapter_client_daemon_changed(
    NfcDaemonClient* daemon,
    NFC_DAEMON_PROPERTY property,
    void* user_data)
{
    NfcAdapterClientObject* self = NFC_ADAPTER_CLIENT_OBJECT(user_data);

    nfc_adapter_client_update(self);
    nfc_adapter_client_emit_queued_signals(self);
}

static
void
nfc_adapter_client_enabled_changed(
    OrgSailfishosNfcAdapter* proxy,
    gboolean enabled,
    NfcAdapterClientObject* self)
{
    NfcAdapterClient* pub = &self->pub;

    /* We may receive a signal before the initial query has completed */
    GASSERT(!self->proxy || self->proxy == proxy);
    if (pub->enabled != enabled) {
        pub->enabled = enabled;
        GDEBUG("%s: %sabled", self->name, enabled ? "En" : "Dis");
        nfc_adapter_client_queue_signal(self, ENABLED);
        nfc_adapter_client_emit_queued_signals(self);
    }
}

static
void
nfc_adapter_client_powered_changed(
    OrgSailfishosNfcAdapter* proxy,
    gboolean powered,
    NfcAdapterClientObject* self)
{
    NfcAdapterClient* pub = &self->pub;

    /* We may receive a signal before the initial query has completed */
    GASSERT(!self->proxy || self->proxy == proxy);
    if (pub->powered != powered) {
        pub->powered = powered;
        GDEBUG("%s: Powered = %s", self->name, powered ? "on" : "off");
        nfc_adapter_client_queue_signal(self, POWERED);
        nfc_adapter_client_emit_queued_signals(self);
    }
}

static
void
nfc_adapter_client_mode_changed(
    OrgSailfishosNfcAdapter* proxy,
    NFC_ADAPTER_MODE mode,
    NfcAdapterClientObject* self)
{
    NfcAdapterClient* pub = &self->pub;

    /* We may receive a signal before the initial query has completed */
    GASSERT(!self->proxy || self->proxy == proxy);
    if (pub->mode != mode) {
        pub->mode = mode;
        GDEBUG("%s: Mode = 0x%02X", self->name, mode);
        nfc_adapter_client_queue_signal(self, MODE);
        nfc_adapter_client_emit_queued_signals(self);
    }
}

static
void
nfc_adapter_client_target_present_changed(
    OrgSailfishosNfcAdapter* proxy,
    gboolean present,
    NfcAdapterClientObject* self)
{
    NfcAdapterClient* pub = &self->pub;

    /* We may receive a signal before the initial query has completed */
    GASSERT(!self->proxy || self->proxy == proxy);
    if (pub->target_present != present) {
        pub->target_present = present;
        GDEBUG("%s: Target = %sresent", self->name, present ? "P" : "Not p");
        nfc_adapter_client_queue_signal(self, TARGET_PRESENT);
        nfc_adapter_client_emit_queued_signals(self);
    }
}

static
void
nfc_adapter_client_tags_changed(
    OrgSailfishosNfcAdapter* proxy,
    GStrV* tags,
    NfcAdapterClientObject* self)
{
    /* We may receive a signal before the initial query has completed */
    GASSERT(!self->proxy || self->proxy == proxy);
    if (!gutil_strv_equal(self->tags, tags)) {
        NfcAdapterClient* pub = &self->pub;

        DUMP_STRV(self->name, "Tags", "=", tags);
        g_strfreev(self->tags);
        pub->tags = self->tags = g_strdupv(tags);
        nfc_adapter_client_queue_signal(self, TAGS);
        nfc_adapter_client_emit_queued_signals(self);
    }
}

static
void
nfc_adapter_client_init_4(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcAdapterClientObject* self = NFC_ADAPTER_CLIENT_OBJECT(user_data);
    NfcAdapterClient* adapter = &self->pub;
    GError* error = NULL;
    gint version;
    guint supported_modes, mode;
    gboolean enabled, powered, target_present;
    gchar** tags;

    GASSERT(self->proxy_initializing);
    self->proxy_initializing = FALSE;
    if (org_sailfishos_nfc_adapter_call_get_all_finish(self->proxy, &version,
        &enabled, &powered, &supported_modes, &mode, &target_present, &tags,
        result, &error)) {
        if (adapter->enabled != enabled) {
            adapter->enabled = enabled;
            nfc_adapter_client_queue_signal(self, ENABLED);
        }
        if (adapter->powered != powered) {
            adapter->powered = powered;
            nfc_adapter_client_queue_signal(self, POWERED);
        }
        adapter->supported_modes = supported_modes;
        if (adapter->mode != mode) {
            adapter->mode = mode;
            nfc_adapter_client_queue_signal(self, MODE);
        }
        if (adapter->target_present != target_present) {
            adapter->target_present = target_present;
            nfc_adapter_client_queue_signal(self, TARGET_PRESENT);
        }
        if (gutil_strv_equal(self->tags, tags)) {
            g_strfreev(tags);
        } else {
            g_strfreev(self->tags);
            adapter->tags = self->tags = tags;
            nfc_adapter_client_queue_signal(self, TAGS);
        }
    } else {
        GERR("%s", GERRMSG(error));
        g_error_free(error);
        /* Need to retry? */
        nfc_adapter_client_drop_proxy(self);
    }
    nfc_adapter_client_update_valid_and_present(self);
    nfc_adapter_client_emit_queued_signals(self);
    g_object_unref(self);
}

static
void
nfc_adapter_client_init_3(
    GObject* connection,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcAdapterClientObject* self = NFC_ADAPTER_CLIENT_OBJECT(user_data);
    GError* error = NULL;

    GASSERT(!self->proxy);
    GASSERT(self->proxy_initializing);
    self->proxy = org_sailfishos_nfc_adapter_proxy_new_finish(result, &error);
    if (self->proxy) {
        self->proxy_signal_id[PROXY_ENABLED_CHANGED] =
            g_signal_connect(self->proxy, "enabled-changed",
                G_CALLBACK(nfc_adapter_client_enabled_changed), self);
        self->proxy_signal_id[PROXY_POWERED_CHANGED] =
            g_signal_connect(self->proxy, "powered-changed",
                G_CALLBACK(nfc_adapter_client_powered_changed), self);
        self->proxy_signal_id[PROXY_MODE_CHANGED] =
            g_signal_connect(self->proxy, "mode-changed",
                G_CALLBACK(nfc_adapter_client_mode_changed), self);
        self->proxy_signal_id[PROXY_TARGET_PRESENT_CHANGED] =
            g_signal_connect(self->proxy, "target-present-changed",
                G_CALLBACK(nfc_adapter_client_target_present_changed), self);
        self->proxy_signal_id[PROXY_TAGS_CHANGED] =
            g_signal_connect(self->proxy, "tags-changed",
                G_CALLBACK(nfc_adapter_client_tags_changed), self);
        org_sailfishos_nfc_adapter_call_get_all(self->proxy, NULL,
            nfc_adapter_client_init_4, g_object_ref(self));
    } else {
        GERR("%s", GERRMSG(error));
        g_error_free(error);
        self->proxy_initializing = FALSE;
        nfc_adapter_client_update_valid_and_present(self);
        nfc_adapter_client_emit_queued_signals(self);
    }
    g_object_unref(self);
}

static
void
nfc_adapter_client_init_2(
    NfcAdapterClientObject* self)
{
    org_sailfishos_nfc_adapter_proxy_new(self->connection,
        G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, NFCD_DBUS_DAEMON_NAME,
        self->pub.path, NULL, nfc_adapter_client_init_3, g_object_ref(self));
}

static
void
nfc_adapter_client_init_1(
    GObject* object,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcAdapterClientObject* self = NFC_ADAPTER_CLIENT_OBJECT(user_data);
    GError* error = NULL;

    GASSERT(self->proxy_initializing);
    self->connection = g_bus_get_finish(result, &error);
    if (self->connection) {
        GDEBUG("Bus connected");
        nfc_adapter_client_init_2(self);
    } else {
        GERR("Failed to attach to NFC daemon bus: %s", GERRMSG(error));
        g_error_free(error);
        self->proxy_initializing = FALSE;
        nfc_adapter_client_update_valid_and_present(self);
        nfc_adapter_client_emit_queued_signals(self);
    }
    g_object_unref(self);
}
        
static
void
nfc_adapter_client_reinit(
    NfcAdapterClientObject* self)
{
    GASSERT(!self->proxy_initializing);
    self->proxy_initializing = TRUE;
    nfc_adapter_client_init_2(self);
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

GDBusConnection*
nfc_adapter_client_connection(
    NfcAdapterClient* adapter)
{
    NfcAdapterClientObject* self = nfc_adapter_client_object_cast(adapter);

    return G_LIKELY(self) ? self->connection : NULL;
}

/*==========================================================================*
 * API
 *==========================================================================*/

NfcAdapterClient*
nfc_adapter_client_new(
    const char* path)
{
    if (G_LIKELY(path) && g_variant_is_object_path(path)) {
        NfcAdapterClientObject* obj = NULL;

        if (nfc_adapter_client_table) {
            obj = g_hash_table_lookup(nfc_adapter_client_table, path);
        }
        if (obj) {
            g_object_ref(obj);
        } else {
            char* key = g_strdup(path);

            GVERBOSE_("%s", path);
            obj = g_object_new(NFC_CLIENT_TYPE_ADAPTER, NULL);
            if (!nfc_adapter_client_table) {
                nfc_adapter_client_table = g_hash_table_new_full(g_str_hash,
                    g_str_equal, g_free, NULL);
            }
            g_hash_table_insert(nfc_adapter_client_table, key, obj);
            obj->pub.path = key;
            obj->name = key + 1;
            nfc_adapter_client_update(obj);
            if (obj->connection) {
                /* Already attached to the bus */
                g_object_ref(obj->connection);
                nfc_adapter_client_init_2(obj);
            } else {
                g_bus_get(NFCD_DBUS_TYPE, NULL, nfc_adapter_client_init_1,
                    g_object_ref(obj));
            }
        }
        return &obj->pub;
    }
    return NULL;
}

NfcAdapterClient*
nfc_adapter_client_ref(
    NfcAdapterClient* adapter)
{
    if (G_LIKELY(adapter)) {
        g_object_ref(nfc_adapter_client_object_cast(adapter));
        return adapter;
    } else {
        return NULL;
    }
}

void
nfc_adapter_client_unref(
    NfcAdapterClient* adapter)
{
    if (G_LIKELY(adapter)) {
        g_object_unref(nfc_adapter_client_object_cast(adapter));
    }
}

gulong
nfc_adapter_client_add_property_handler(
    NfcAdapterClient* adapter,
    NFC_ADAPTER_PROPERTY property,
    NfcAdapterPropertyFunc callback,
    void* user_data)
{
    NfcAdapterClientObject* self = nfc_adapter_client_object_cast(adapter);

    return G_LIKELY(self) ? nfc_client_base_add_property_handler(&self->base,
        property, (NfcClientBasePropertyFunc) callback, user_data) : 0;
}

void
nfc_adapter_client_remove_handler(
    NfcAdapterClient* adapter,
    gulong id)
{
    if (G_LIKELY(id)) {
        NfcAdapterClientObject* self = nfc_adapter_client_object_cast(adapter);

        if (G_LIKELY(self)) {
            g_signal_handler_disconnect(self, id);
        }
    }
}

void
nfc_adapter_client_remove_handlers(
    NfcAdapterClient* adapter,
    gulong* ids,
    guint n)
{
    gutil_disconnect_handlers(nfc_adapter_client_object_cast(adapter), ids, n);
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
nfc_adapter_client_object_init(
    NfcAdapterClientObject* self)
{
    NfcAdapterClient* pub = &self->pub;

    pub->tags = &nfc_adapter_client_empty_strv;
    self->daemon = nfc_daemon_client_new();
    self->daemon_event_id[DAEMON_VALID_CHANGED] =
        nfc_daemon_client_add_property_handler(self->daemon,
            NFC_DAEMON_PROPERTY_VALID,
            nfc_adapter_client_daemon_changed, self);
    self->daemon_event_id[DAEMON_ADAPTERS_CHANGED] =
        nfc_daemon_client_add_property_handler(self->daemon,
            NFC_DAEMON_PROPERTY_ADAPTERS,
            nfc_adapter_client_daemon_changed, self);
    self->proxy_initializing = TRUE;
    self->connection = nfc_daemon_client_connection(self->daemon);
    if (self->connection) {
        g_object_ref(self->connection);
    }
}

static
void
nfc_adapter_client_object_finalize(
    GObject* object)
{
    NfcAdapterClientObject* self = NFC_ADAPTER_CLIENT_OBJECT(object);
    NfcAdapterClient* pub = &self->pub;

    GVERBOSE_("%s", pub->path);
    nfc_adapter_client_drop_proxy(self);
    nfc_daemon_client_remove_all_handlers(self->daemon, self->daemon_event_id);
    nfc_daemon_client_unref(self->daemon);
    if (self->connection) {
        g_object_unref(self->connection);
    }
    g_strfreev(self->tags);
    g_hash_table_remove(nfc_adapter_client_table, pub->path);
    if (g_hash_table_size(nfc_adapter_client_table) == 0) {
        g_hash_table_unref(nfc_adapter_client_table);
        nfc_adapter_client_table = NULL;
    }
    G_OBJECT_CLASS(nfc_adapter_client_object_parent_class)->finalize(object);
}

static
void
nfc_adapter_client_object_class_init(
    NfcAdapterClientObjectClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = nfc_adapter_client_object_finalize;
    klass->public_offset = G_STRUCT_OFFSET(NfcAdapterClientObject, pub);
    klass->valid_offset = G_STRUCT_OFFSET(NfcAdapterClientObject, pub.valid);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
