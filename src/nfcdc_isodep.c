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

#include "nfcdc_isodep.h"
#include "nfcdc_base.h"
#include "nfcdc_dbus.h"
#include "nfcdc_log.h"
#include "nfcdc_tag_p.h"
#include "nfcdc_util_p.h"

#include <gutil_macros.h>
#include <gutil_misc.h>
#include <gutil_strv.h>

#include "org.sailfishos.nfc.IsoDep.h"

typedef NfcClientBaseClass NfcIsoDepClientObjectClass;
typedef struct nfc_isodep_client_object {
    NfcClientBase base;
    NfcIsoDepClient pub;
    NfcTagClient* tag;
    gulong tag_event_id;
    GDBusConnection* connection;
    OrgSailfishosNfcIsoDep* proxy;
    GHashTable* act_params;
    gboolean proxy_initializing;
    gint version;
    const char* name;
} NfcIsoDepClientObject;

#define PARENT_CLASS nfc_isodep_client_object_parent_class
#define THIS_TYPE nfc_isodep_client_object_get_type()
#define THIS(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, THIS_TYPE, \
    NfcIsoDepClientObject)

GType THIS_TYPE G_GNUC_INTERNAL;
G_DEFINE_TYPE(NfcIsoDepClientObject, nfc_isodep_client_object, \
    NFC_CLIENT_TYPE_BASE)

NFC_CLIENT_BASE_ASSERT_VALID(NFC_ISODEP_PROPERTY_VALID);
NFC_CLIENT_BASE_ASSERT_COUNT(NFC_ISODEP_PROPERTY_COUNT);

#define SIGNAL_BIT_(x) NFC_CLIENT_BASE_SIGNAL_BIT(NFC_ISODEP_PROPERTY_##x)

#define nfc_isodep_client_emit_queued_signals(self) \
    nfc_client_base_emit_queued_signals(&(self)->base)
#define nfc_isodep_client_queue_signal(self,NAME) \
    ((self)->base.queued_signals |= SIGNAL_BIT_(NAME))

typedef struct nfc_isodep_transmit_data {
    NfcIsoDepClientObject* object;
    NfcIsoDepTransmitFunc callback;
    GDestroyNotify destroy;
    void* user_data;
    GCancellable* cancel;
    gulong cancel_id;
} NfcIsoDepTransmitData;

#define NFC_ISODEP_ACT_PARAM_UNKNOWN NFC_ISODEP_ACT_PARAM_COUNT

static GHashTable* nfc_isodep_client_table;

static
void
nfc_isodep_client_reinit(
    NfcIsoDepClientObject* self);

/*==========================================================================*
 * Implementation
 *==========================================================================*/

static inline
NfcIsoDepClientObject*
nfc_isodep_client_object_cast(
    NfcIsoDepClient* pub)
{
    return G_LIKELY(pub) ?
        THIS(G_CAST(pub, NfcIsoDepClientObject, pub)) :
        NULL;
}

static
int
nfc_isodep_client_act_param_key(
    const char* key)
{
    if (key) {
        if (!strcmp(key, "T0")) {
            return NFC_ISODEP_ACT_PARAM_T0;
        } else if (!strcmp(key, "TA")) {
            return NFC_ISODEP_ACT_PARAM_TA;
        } else if (!strcmp(key, "TB")) {
            return NFC_ISODEP_ACT_PARAM_TB;
        } else if (!strcmp(key, "TC")) {
            return NFC_ISODEP_ACT_PARAM_TC;
        } else if (!strcmp(key, "HB")) {
            return NFC_ISODEP_ACT_PARAM_HB;
        } else if (!strcmp(key, "MBLI")) {
            return NFC_ISODEP_ACT_PARAM_MBLI;
        } else if (!strcmp(key, "DID")) {
            return NFC_ISODEP_ACT_PARAM_DID;
        } else if (!strcmp(key, "HLR")) {
            return NFC_ISODEP_ACT_PARAM_HLR;
        }
    }
    return -1;
}

static
void
nfc_isodep_client_transmit_cancelled(
    GCancellable* cancel,
    NfcIsoDepTransmitData* data)
{
    data->callback = NULL;
}

static
void
nfc_isodep_client_transmit_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcIsoDepTransmitData* data = user_data;
    GVariant* response = NULL;
    GError* error = NULL;
    guchar sw1 = 0, sw2 = 0;

    org_sailfishos_nfc_iso_dep_call_transmit_finish(ORG_SAILFISHOS_NFC_ISO_DEP
        (proxy), &response, &sw1, &sw2, result, &error);
    if (data->cancel) {
        g_signal_handler_disconnect(data->cancel, data->cancel_id);
        g_object_unref(data->cancel);
    }
    if (data->callback) {
        NfcIsoDepTransmitFunc callback = data->callback;
        NfcIsoDepClient* isodep = &data->object->pub;
        const GUtilData* resp;
        GUtilData d;

        if (error) {
            resp = NULL;
        } else {
            d.bytes = g_variant_get_fixed_array(response, &d.size, 1);
            resp = &d;
        }
        data->callback = NULL;
        callback(isodep, resp, NFC_ISODEP_SW(sw1, sw2), error, data->user_data);
    }
    if (data->destroy) {
        data->destroy(data->user_data);
    }
    if (error) {
        g_error_free(error);
    } else {
        g_variant_unref(response);
    }
    g_object_unref(data->object);
    gutil_slice_free(data);
}

static
void
nfc_isodep_client_update_valid_and_present(
    NfcIsoDepClientObject* self)
{
    NfcIsoDepClient* pub = &self->pub;
    NfcTagClient* tag = self->tag;
    gboolean valid, present;

    if (!tag->valid || self->proxy_initializing) {
        valid = FALSE;
        present = FALSE;
    } else {
        valid = TRUE;
        present = (self->proxy && tag->present &&
            gutil_strv_contains(tag->interfaces, NFC_TAG_INTERFACE_ISODEP));
    }
    if (pub->valid != valid) {
        pub->valid = valid;
        nfc_isodep_client_queue_signal(self, VALID);
    }
    if (pub->present != present) {
        pub->present = present;
        nfc_isodep_client_queue_signal(self, PRESENT);
    }
}

static
void
nfc_isodep_client_drop_proxy(
    NfcIsoDepClientObject* self)
{
    NfcIsoDepClient* pub = &self->pub;

    GASSERT(!self->proxy_initializing);
    if (self->proxy) {
        g_object_unref(self->proxy);
        self->proxy = NULL;
    }
    if (pub->valid) {
        pub->valid = FALSE;
        nfc_isodep_client_queue_signal(self, VALID);
    }
    if (pub->present) {
        pub->present = FALSE;
        nfc_isodep_client_queue_signal(self, PRESENT);
    }
}

static
void
nfc_isodep_client_update(
    NfcIsoDepClientObject* self)
{
    if (gutil_strv_contains(self->tag->interfaces, NFC_TAG_INTERFACE_ISODEP)) {
        if (!self->proxy && !self->proxy_initializing) {
            nfc_isodep_client_reinit(self);
        }
    } else if (!self->proxy_initializing) {
        nfc_isodep_client_drop_proxy(self);
    }
    nfc_isodep_client_update_valid_and_present(self);
}

static
void
nfc_isodep_client_tag_changed(
    NfcTagClient* tag,
    NFC_TAG_PROPERTY property,
    void* user_data)
{
    NfcIsoDepClientObject* self = THIS(user_data);

    nfc_isodep_client_update(self);
    nfc_isodep_client_emit_queued_signals(self);
}

static
void
nfc_isodep_client_init_5(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcIsoDepClientObject* self = THIS(user_data);
    GError* error = NULL;
    GVariant* dict = NULL;
    int version = 0;

    GASSERT(self->proxy_initializing);
    self->proxy_initializing = FALSE;
    if (!org_sailfishos_nfc_iso_dep_call_get_all2_finish(self->proxy,
        &version, &dict, result, &error)) {
        GERR("%s", GERRMSG(error));
        g_error_free(error);
        /* Need to retry? */
        nfc_isodep_client_drop_proxy(self);
    } else {
        GDEBUG("%s: ISO-DEP activation parameters", self->name);
        self->act_params = nfc_parse_dict(self->act_params, dict,
            nfc_isodep_client_act_param_key);
        self->version = version;
        nfc_isodep_client_update_valid_and_present(self);
        nfc_isodep_client_emit_queued_signals(self);
        g_variant_unref(dict);
    }
    g_object_unref(self);
}

static
void
nfc_isodep_client_init_4(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcIsoDepClientObject* self = THIS(user_data);
    GError* error = NULL;
    int version = 0;

    GASSERT(self->proxy_initializing);
    if (!org_sailfishos_nfc_iso_dep_call_get_all_finish(self->proxy,
        &version, result, &error)) {
        GERR("%s", GERRMSG(error));
        self->proxy_initializing = FALSE;
        g_error_free(error);
        /* Need to retry? */
        nfc_isodep_client_drop_proxy(self);
    } else if (version > 1) {
        /* Version 2 or greater */
        org_sailfishos_nfc_iso_dep_call_get_all2(self->proxy, NULL,
            nfc_isodep_client_init_5, g_object_ref(self));
    } else {
        self->version = version;
        self->proxy_initializing = FALSE;
        nfc_isodep_client_update_valid_and_present(self);
    }
    nfc_isodep_client_emit_queued_signals(self);
    g_object_unref(self);
}

static
void
nfc_isodep_client_init_3(
    GObject* connection,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcIsoDepClientObject* self = THIS(user_data);
    GError* error = NULL;

    GASSERT(!self->proxy);
    GASSERT(self->proxy_initializing);
    self->proxy = org_sailfishos_nfc_iso_dep_proxy_new_finish(result, &error);
    if (self->proxy) {
        org_sailfishos_nfc_iso_dep_call_get_all(self->proxy, NULL,
            nfc_isodep_client_init_4, g_object_ref(self));
    } else {
        GERR("%s", GERRMSG(error));
        g_error_free(error);
        self->proxy_initializing = FALSE;
        nfc_isodep_client_update_valid_and_present(self);
        nfc_isodep_client_emit_queued_signals(self);
    }
    g_object_unref(self);
}

static
void
nfc_isodep_client_init_2(
    NfcIsoDepClientObject* self)
{
    org_sailfishos_nfc_iso_dep_proxy_new(self->connection,
        G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, NFCD_DBUS_DAEMON_NAME,
        self->pub.path, NULL, nfc_isodep_client_init_3, g_object_ref(self));
}

static
void
nfc_isodep_client_init_1(
    GObject* object,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcIsoDepClientObject* self = THIS(user_data);
    GError* error = NULL;

    self->connection = g_bus_get_finish(result, &error);
    if (self->connection) {
        GDEBUG("Bus connected");
        nfc_isodep_client_update(self);
    } else {
        GERR("Failed to attach to NFC daemon bus: %s", GERRMSG(error));
        g_error_free(error);
        nfc_isodep_client_update_valid_and_present(self);
        nfc_isodep_client_emit_queued_signals(self);
    }
    g_object_unref(self);
}
        
static
void
nfc_isodep_client_reinit(
    NfcIsoDepClientObject* self)
{
    GASSERT(!self->proxy_initializing);
    self->proxy_initializing = TRUE;
    nfc_isodep_client_init_2(self);
}

/*==========================================================================*
 * API
 *==========================================================================*/

NfcIsoDepClient*
nfc_isodep_client_new(
    const char* path)
{
    if (G_LIKELY(path) && g_variant_is_object_path(path)) {
        const char* sep = strrchr(path, '/');
        if (sep > path) {
            NfcIsoDepClientObject* obj = NULL;

            if (nfc_isodep_client_table) {
                obj = g_hash_table_lookup(nfc_isodep_client_table, path);
            }
            if (obj) {
                g_object_ref(obj);
            } else {
                char* key = g_strdup(path);

                GVERBOSE_("%s", path);
                obj = g_object_new(THIS_TYPE, NULL);
                if (!nfc_isodep_client_table) {
                    nfc_isodep_client_table = g_hash_table_new_full(g_str_hash,
                        g_str_equal, g_free, NULL);
                }
                g_hash_table_insert(nfc_isodep_client_table, key, obj);
                obj->pub.path = key;
                obj->name = key + ((sep - path) + 1);
                obj->tag = nfc_tag_client_new(path);
                obj->tag_event_id =
                    nfc_tag_client_add_property_handler(obj->tag,
                        NFC_TAG_PROPERTY_VALID,
                        nfc_isodep_client_tag_changed, obj);
                obj->connection = nfc_tag_client_connection(obj->tag);
                if (obj->connection) {
                    /* Already attached to the bus */
                    g_object_ref(obj->connection);
                    nfc_isodep_client_update(obj);
                } else {
                    g_bus_get(NFCD_DBUS_TYPE, NULL, nfc_isodep_client_init_1,
                        g_object_ref(obj));
                }
            }
            return &obj->pub;
        }
    }
    return NULL;
}

NfcIsoDepClient*
nfc_isodep_client_ref(
    NfcIsoDepClient* isodep)
{
    if (G_LIKELY(isodep)) {
        g_object_ref(nfc_isodep_client_object_cast(isodep));
        return isodep;
    } else {
        return NULL;
    }
}

void
nfc_isodep_client_unref(
    NfcIsoDepClient* isodep)
{
    if (G_LIKELY(isodep)) {
        g_object_unref(nfc_isodep_client_object_cast(isodep));
    }
}

NfcTagClient*
nfc_isodep_client_tag(
    NfcIsoDepClient* isodep) /* Since 1.0.10 */
{
    return G_LIKELY(isodep) ?
        nfc_isodep_client_object_cast(isodep)->tag :
        NULL;
}

const GUtilData*
nfc_isodep_client_act_param(
    NfcIsoDepClient* isodep,
    NFC_ISODEP_ACT_PARAM param) /* Since 1.0.8 */
{
    NfcIsoDepClientObject* self = nfc_isodep_client_object_cast(isodep);

    return (self && self->act_params) ?
        g_hash_table_lookup(self->act_params, GINT_TO_POINTER(param)) :
        NULL;
}

gboolean
nfc_isodep_client_transmit(
    NfcIsoDepClient* isodep,
    const NfcIsoDepApdu* apdu,
    GCancellable* cancel,
    NfcIsoDepTransmitFunc callback,
    void* user_data,
    GDestroyNotify destroy)
{
    NfcIsoDepClientObject* self = nfc_isodep_client_object_cast(isodep);

    if (self && apdu && isodep->valid && isodep->present &&
        (callback || destroy) &&
        (!cancel || !g_cancellable_is_cancelled(cancel))) {
        NfcIsoDepTransmitData* data = g_slice_new0(NfcIsoDepTransmitData);
        const void* bytes = apdu->data.bytes;

        if (!bytes) bytes = &data;
        g_object_ref(data->object = self);
        data->callback = callback;
        data->destroy = destroy;
        data->user_data = user_data;
        if (cancel) {
            g_object_ref(data->cancel = cancel);
            data->cancel_id = g_cancellable_connect(cancel,
                G_CALLBACK(nfc_isodep_client_transmit_cancelled), data, NULL);
        }
        org_sailfishos_nfc_iso_dep_call_transmit(self->proxy, apdu->cla,
            apdu->ins, apdu->p1, apdu->p2, g_variant_new_from_data
            (G_VARIANT_TYPE("ay"), bytes, apdu->data.size, TRUE, NULL, NULL),
            apdu->le, cancel, nfc_isodep_client_transmit_done, data);
        return TRUE;
    } else {
        /* Destroy callback is always invoked even if we return FALSE */
        if (destroy) {
            destroy(user_data);
        }
        return FALSE;
    }
}

gulong
nfc_isodep_client_add_property_handler(
    NfcIsoDepClient* isodep,
    NFC_ISODEP_PROPERTY property,
    NfcIsoDepPropertyFunc callback,
    void* user_data)
{
    NfcIsoDepClientObject* self = nfc_isodep_client_object_cast(isodep);

    return G_LIKELY(self) ? nfc_client_base_add_property_handler(&self->base,
        property, (NfcClientBasePropertyFunc) callback, user_data) : 0;
}

void
nfc_isodep_client_remove_handler(
    NfcIsoDepClient* isodep,
    gulong id)
{
    if (G_LIKELY(id)) {
        NfcIsoDepClientObject* self = nfc_isodep_client_object_cast(isodep);

        if (G_LIKELY(self)) {
            g_signal_handler_disconnect(self, id);
        }
    }
}

void
nfc_isodep_client_remove_handlers(
    NfcIsoDepClient* isodep,
    gulong* ids,
    guint n)
{
    gutil_disconnect_handlers(nfc_isodep_client_object_cast(isodep), ids, n);
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
nfc_isodep_client_object_init(
    NfcIsoDepClientObject* self)
{
}

static
void
nfc_isodep_client_object_finalize(
    GObject* object)
{
    NfcIsoDepClientObject* self = THIS(object);
    NfcIsoDepClient* pub = &self->pub;

    GVERBOSE_("%s", pub->path);
    nfc_isodep_client_drop_proxy(self);
    nfc_tag_client_remove_handler(self->tag, self->tag_event_id);
    nfc_tag_client_unref(self->tag);
    if (self->connection) {
        g_object_unref(self->connection);
    }
    if (self->act_params) {
        g_hash_table_destroy(self->act_params);
    }
    g_hash_table_remove(nfc_isodep_client_table, pub->path);
    if (g_hash_table_size(nfc_isodep_client_table) == 0) {
        g_hash_table_unref(nfc_isodep_client_table);
        nfc_isodep_client_table = NULL;
    }
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
nfc_isodep_client_object_class_init(
    NfcIsoDepClientObjectClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = nfc_isodep_client_object_finalize;
    klass->public_offset = G_STRUCT_OFFSET(NfcIsoDepClientObject, pub);
    klass->valid_offset = G_STRUCT_OFFSET(NfcIsoDepClientObject, pub.valid);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
