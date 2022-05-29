/*
 * Copyright (C) 2019-2022 Jolla Ltd.
 * Copyright (C) 2019-2022 Slava Monich <slava@monich.com>
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

#include "nfcdc_default_adapter.h"
#include "nfcdc_daemon.h"
#include "nfcdc_base.h"
#include "nfcdc_log.h"

#include <gutil_macros.h>
#include <gutil_misc.h>
#include <gutil_strv.h>

enum nfc_default_adapter_daemon_signals {
    DAEMON_VALID_CHANGED,
    DAEMON_ADAPTERS_CHANGED,
    DAEMON_SIGNAL_COUNT
};

typedef NfcClientBaseClass NfcDefaultAdapterObjectClass;
typedef struct nfc_default_adapter_object {
    NfcClientBase base;
    NfcDefaultAdapter pub;
    NfcDaemonClient* daemon;
    NfcAdapterClient* adapter;
    gulong daemon_event_id[DAEMON_SIGNAL_COUNT];
    gulong adapter_signal_id;
    GStrV* tags;
    GStrV* peers;
} NfcDefaultAdapterObject;

#define PARENT_CLASS nfc_default_adapter_object_parent_class
#define THIS_TYPE nfc_default_adapter_object_get_type()
#define THIS(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, THIS_TYPE, \
    NfcDefaultAdapterObject)

GType THIS_TYPE G_GNUC_INTERNAL;
G_DEFINE_TYPE(NfcDefaultAdapterObject, nfc_default_adapter_object, \
    NFC_CLIENT_TYPE_BASE)

NFC_CLIENT_BASE_ASSERT_COUNT(NFC_DEFAULT_ADAPTER_PROPERTY_COUNT);

#define SIGNAL_BIT_(x) \
    NFC_CLIENT_BASE_SIGNAL_BIT(NFC_DEFAULT_ADAPTER_PROPERTY_##x)

#define nfc_default_adapter_emit_queued_signals(self) \
    nfc_client_base_emit_queued_signals(&(self)->base)
#define nfc_default_adapter_queue_signal(self,NAME) \
    ((self)->base.queued_signals |= SIGNAL_BIT_(NAME))

static char* nfc_default_adapter_empty_strv = NULL;
static NfcDefaultAdapterObject* nfc_default_adapter_instance = NULL;

/*==========================================================================*
 * Implementation
 *==========================================================================*/

static inline
NfcDefaultAdapterObject*
nfc_default_adapter_object_cast(
    NfcDefaultAdapter* pub)
{
    return G_LIKELY(pub) ?
        THIS(G_CAST(pub, NfcDefaultAdapterObject, pub)) :
        NULL;
}

static
void
nfc_default_adapter_drop_adapter(
    NfcDefaultAdapterObject* self)
{
    if (self->adapter) {
        nfc_adapter_client_remove_handlers(self->adapter,
            &self->adapter_signal_id, 1);
        nfc_adapter_client_unref(self->adapter);
        self->adapter = NULL;
    }
}

static
void
nfc_default_adapter_clear(
    NfcDefaultAdapterObject* self)
{
    NfcDefaultAdapter* pub = &self->pub;

    if (pub->adapter) {
        pub->adapter = NULL;
        nfc_default_adapter_queue_signal(self, ADAPTER);
    }
    if (pub->enabled) {
        pub->enabled = FALSE;
        nfc_default_adapter_queue_signal(self, ENABLED);
    }
    if (pub->powered) {
        pub->powered = FALSE;
        nfc_default_adapter_queue_signal(self, POWERED);
    }
    if (pub->supported_modes) {
        pub->supported_modes = NFC_MODE_NONE;
        nfc_default_adapter_queue_signal(self, SUPPORTED_MODES);
    }
    if (pub->mode) {
        pub->mode = NFC_MODE_NONE;
        nfc_default_adapter_queue_signal(self, MODE);
    }
    if (pub->target_present) {
        pub->target_present = FALSE;
        nfc_default_adapter_queue_signal(self, TARGET_PRESENT);
    }
    if (pub->tags[0]) {
        g_strfreev(self->tags);
        self->tags = NULL;
        pub->tags = &nfc_default_adapter_empty_strv;
        nfc_default_adapter_queue_signal(self, TAGS);
    }
    if (pub->peers[0]) {
        g_strfreev(self->peers);
        self->peers = NULL;
        pub->peers = &nfc_default_adapter_empty_strv;
        nfc_default_adapter_queue_signal(self, PEERS);
    }
}

static
void
nfc_default_adapter_sync(
    NfcDefaultAdapterObject* self)
{
    NfcAdapterClient* adapter = self->adapter;
    NfcDefaultAdapter* pub = &self->pub;

    if (adapter->valid && adapter->present) {
        if (pub->adapter != adapter) {
            pub->adapter = adapter;
            nfc_default_adapter_queue_signal(self, ADAPTER);
        }
    } else if (pub->adapter) {
        pub->adapter = NULL;
        nfc_default_adapter_queue_signal(self, ADAPTER);
    }
    if (pub->enabled != adapter->enabled) {
        pub->enabled = adapter->enabled;
        nfc_default_adapter_queue_signal(self, ENABLED);
    }
    if (pub->powered != adapter->powered) {
        pub->powered = adapter->powered;
        nfc_default_adapter_queue_signal(self, POWERED);
    }
    if (pub->supported_modes != adapter->supported_modes) {
        pub->supported_modes = adapter->supported_modes;
        nfc_default_adapter_queue_signal(self, SUPPORTED_MODES);
    }
    if (pub->mode != adapter->mode) {
        pub->mode = adapter->mode;
        nfc_default_adapter_queue_signal(self, MODE);
    }
    if (pub->target_present != adapter->target_present) {
        pub->target_present = adapter->target_present;
        nfc_default_adapter_queue_signal(self, TARGET_PRESENT);
    }
    if (!gutil_strv_equal(pub->tags, adapter->tags)) {
        g_strfreev(self->tags);
        if (adapter->tags[0]) {
            pub->tags = self->tags = g_strdupv((char**)adapter->tags);
        } else {
            self->tags = NULL;
            pub->tags = &nfc_default_adapter_empty_strv;
        }
        nfc_default_adapter_queue_signal(self, TAGS);
    }
    if (!gutil_strv_equal(pub->peers, adapter->peers)) {
        g_strfreev(self->peers);
        if (adapter->peers[0]) {
            pub->peers = self->peers = g_strdupv((char**)adapter->peers);
        } else {
            self->peers = NULL;
            pub->peers = &nfc_default_adapter_empty_strv;
        }
        nfc_default_adapter_queue_signal(self, PEERS);
    }
}

static
void
nfc_default_adapter_property_changed(
    NfcAdapterClient* adapter,
    NFC_ADAPTER_PROPERTY property,
    void* user_data)
{
    NfcDefaultAdapterObject* self = THIS(user_data);

    nfc_default_adapter_sync(self);
    nfc_default_adapter_emit_queued_signals(self);
}

static
void
nfc_default_adapter_check(
    NfcDefaultAdapterObject* self)
{
    const char* adapter_path = self->daemon->adapters[0];

    if (adapter_path) {
        if (!self->adapter || strcmp(self->adapter->path, adapter_path)) {
            nfc_default_adapter_drop_adapter(self);
            self->adapter = nfc_adapter_client_new(adapter_path);
            self->adapter_signal_id =
                nfc_adapter_client_add_property_handler(self->adapter,
                    NFC_ADAPTER_PROPERTY_ANY,
                    nfc_default_adapter_property_changed, self);
            nfc_default_adapter_sync(self);
        }
    } else {
        nfc_default_adapter_drop_adapter(self);
        nfc_default_adapter_clear(self);
    }
}

static
void
nfc_default_adapter_daemon_valid_changed(
    NfcDaemonClient* daemon,
    NFC_DAEMON_PROPERTY property,
    void* user_data)
{
    NfcDefaultAdapterObject* self = THIS(user_data);
    NfcDefaultAdapter* pub = &self->pub;

    pub->valid = daemon->valid;
    nfc_default_adapter_queue_signal(self, VALID);
    nfc_default_adapter_emit_queued_signals(self);
}

static
void
nfc_default_adapter_daemon_adapters_changed(
    NfcDaemonClient* daemon,
    NFC_DAEMON_PROPERTY property,
    void* user_data)
{
    NfcDefaultAdapterObject* self = THIS(user_data);

    nfc_default_adapter_check(self);
    nfc_default_adapter_emit_queued_signals(self);
}

/*==========================================================================*
 * API
 *==========================================================================*/

NfcDefaultAdapter*
nfc_default_adapter_new()
{
    if (nfc_default_adapter_instance) {
        g_object_ref(nfc_default_adapter_instance);
    } else {
        nfc_default_adapter_instance = g_object_new(THIS_TYPE, NULL);
    }
    return &nfc_default_adapter_instance->pub;
}

NfcDefaultAdapter*
nfc_default_adapter_ref(
    NfcDefaultAdapter* da)
{
    if (G_LIKELY(da)) {
        g_object_ref(nfc_default_adapter_object_cast(da));
        return da;
    } else {
        return NULL;
    }
}

void
nfc_default_adapter_unref(
    NfcDefaultAdapter* da)
{
    if (G_LIKELY(da)) {
        g_object_unref(nfc_default_adapter_object_cast(da));
    }
}

gulong
nfc_default_adapter_add_property_handler(
    NfcDefaultAdapter* da,
    NFC_DEFAULT_ADAPTER_PROPERTY property,
    NfcDefaultAdapterPropertyFunc callback,
    void* user_data)
{
    NfcDefaultAdapterObject* self = nfc_default_adapter_object_cast(da);

    return G_LIKELY(self) ? nfc_client_base_add_property_handler(&self->base,
        property, (NfcClientBasePropertyFunc) callback, user_data) : 0;
}

void
nfc_default_adapter_remove_handler(
    NfcDefaultAdapter* da,
    gulong id)
{
    if (G_LIKELY(id)) {
        NfcDefaultAdapterObject* self = nfc_default_adapter_object_cast(da);

        if (G_LIKELY(self)) {
            g_signal_handler_disconnect(self, id);
        }
    }
}

void
nfc_default_adapter_remove_handlers(
    NfcDefaultAdapter* da,
    gulong* ids,
    guint n)
{
    gutil_disconnect_handlers(nfc_default_adapter_object_cast(da), ids, n);
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
nfc_default_adapter_object_init(
    NfcDefaultAdapterObject* self)
{
    NfcDefaultAdapter* pub = &self->pub;

    GVERBOSE_("");
    pub->tags = &nfc_default_adapter_empty_strv;
    pub->peers = &nfc_default_adapter_empty_strv;
    self->daemon = nfc_daemon_client_new();
    self->daemon_event_id[DAEMON_VALID_CHANGED] =
        nfc_daemon_client_add_property_handler(self->daemon,
            NFC_DAEMON_PROPERTY_VALID,
            nfc_default_adapter_daemon_valid_changed, self);
    self->daemon_event_id[DAEMON_ADAPTERS_CHANGED] =
        nfc_daemon_client_add_property_handler(self->daemon,
            NFC_DAEMON_PROPERTY_ADAPTERS,
            nfc_default_adapter_daemon_adapters_changed, self);
    pub->valid = self->daemon->valid;
    nfc_default_adapter_check(self);
}

static
void
nfc_default_adapter_object_finalize(
    GObject* object)
{
    NfcDefaultAdapterObject* self = THIS(object);

    GVERBOSE_("");
    GASSERT(nfc_default_adapter_instance == self);
    nfc_default_adapter_instance = NULL;
    nfc_default_adapter_drop_adapter(self);
    nfc_daemon_client_remove_all_handlers(self->daemon, self->daemon_event_id);
    nfc_daemon_client_unref(self->daemon);
    g_strfreev(self->tags);
    g_strfreev(self->peers);
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
nfc_default_adapter_object_class_init(
    NfcDefaultAdapterObjectClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = nfc_default_adapter_object_finalize;
    klass->public_offset = G_STRUCT_OFFSET(NfcDefaultAdapterObject, pub);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
