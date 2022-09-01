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

#include "nfcdc_adapter_p.h"
#include "nfcdc_base.h"
#include "nfcdc_dbus.h"
#include "nfcdc_log.h"
#include "nfcdc_tag_p.h"
#include "nfcdc_util_p.h"

#include <gutil_macros.h>
#include <gutil_misc.h>
#include <gutil_strv.h>

#include "org.sailfishos.nfc.Tag.h"

enum nfc_tag_client_adapter_signals {
    ADAPTER_VALID_CHANGED,
    ADAPTER_TAGS_CHANGED,
    ADAPTER_SIGNAL_COUNT
};

typedef NfcClientBaseClass NfcTagClientObjectClass;
typedef struct nfc_tag_client_object {
    NfcClientBase base;
    NfcTagClient pub;
    NfcTagClientLock* lock;
    GDBusConnection* connection;
    NfcAdapterClient* adapter;
    gulong adapter_event_id[ADAPTER_SIGNAL_COUNT];
    OrgSailfishosNfcTag* proxy;
    GHashTable* poll_params;
    gboolean proxy_initializing;
    gint version;
    const char* name;
    GStrV* interfaces;
    GStrV* ndef_records;
} NfcTagClientObject;

#define PARENT_CLASS nfc_tag_client_object_parent_class
#define THIS_TYPE nfc_tag_client_object_get_type()
#define THIS(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, THIS_TYPE, \
    NfcTagClientObject)

GType THIS_TYPE G_GNUC_INTERNAL;
G_DEFINE_TYPE(NfcTagClientObject, nfc_tag_client_object, \
    NFC_CLIENT_TYPE_BASE)

NFC_CLIENT_BASE_ASSERT_VALID(NFC_TAG_PROPERTY_VALID);
NFC_CLIENT_BASE_ASSERT_COUNT(NFC_TAG_PROPERTY_COUNT);

#define SIGNAL_BIT_(x) NFC_CLIENT_BASE_SIGNAL_BIT(NFC_TAG_PROPERTY_##x)

#define nfc_tag_client_emit_queued_signals(self) \
    nfc_client_base_emit_queued_signals(&(self)->base)
#define nfc_tag_client_queue_signal(self,NAME) \
    ((self)->base.queued_signals |= SIGNAL_BIT_(NAME))

typedef struct nfc_tag_client_call_data {
    NfcTagClientObject* obj;
    NfcTagClientCallFunc callback;
    gboolean (*finish)(OrgSailfishosNfcTag*, GAsyncResult*, GError**);
    GDestroyNotify destroy;
    void* user_data;
    GCancellable* cancel;
    gulong cancel_id;
} NfcTagClientCallData;

static char* nfc_tag_client_empty_strv = NULL;
static GHashTable* nfc_tag_client_table;

static
void
nfc_tag_client_reinit(
    NfcTagClientObject* self);

/*==========================================================================*
 * Lock
 *==========================================================================*/

struct nfc_tag_client_lock {
    NfcTagClientObject* tag;
    gint ref_count;
};

typedef struct nfc_tag_client_lock_data {
    NfcTagClientObject* tag;
    NfcTagClientLockFunc callback;
    GDestroyNotify destroy;
    void* user_data;
    GCancellable* cancel;
    gulong cancel_id;
} NfcTagClientLockData;

typedef struct nfc_tag_client_lock_data_idle {
    NfcTagClientLockData data;
    guint id;
} NfcTagClientLockDataIdle;

static
void
nfc_tag_client_lock_cancelled(
    GCancellable* cancel,
    NfcTagClientLockData* data)
{
    data->callback = NULL;
}

static
void
nfc_tag_client_lock_data_init(
    NfcTagClientLockData* data,
    NfcTagClientObject* tag,
    NfcTagClientLockFunc callback,
    GDestroyNotify destroy,
    void* user_data,
    GCancellable* cancel)
{
    /* The structure is supposed to be zero-initialized by the caller */
    g_object_ref(data->tag = tag);
    data->callback = callback;
    data->destroy = destroy;
    data->user_data = user_data;
    if (cancel) {
        g_object_ref(data->cancel = cancel);
        data->cancel_id = g_cancellable_connect(cancel,
            G_CALLBACK(nfc_tag_client_lock_cancelled), data, NULL);
    }
}

static
void
nfc_tag_client_lock_data_deinit(
    NfcTagClientLockData* data)
{
    if (data->cancel) {
        g_signal_handler_disconnect(data->cancel, data->cancel_id);
        g_object_unref(data->cancel);
    }
    if (data->destroy) {
        data->destroy(data->user_data);
    }
    g_object_unref(data->tag);
}

static
void
nfc_tag_client_lock_data_idle_free(
    gpointer user_data)
{
    NfcTagClientLockDataIdle* idle = user_data;
    NfcTagClientLockData* data = &idle->data;
    NfcTagClientObject* tag = data->tag;

    GASSERT(tag->lock);
    nfc_tag_client_lock_unref(tag->lock);
    nfc_tag_client_lock_data_deinit(data);
    gutil_slice_free(idle);
}

static
gboolean
nfc_tag_client_lock_idle_callback(
    gpointer user_data)
{
    NfcTagClientLockDataIdle* idle = user_data;
    NfcTagClientLockData* data = &idle->data;
    NfcTagClientObject* tag = data->tag;

    idle->id = 0;
    GASSERT(tag->lock);
    if (data->callback) {
        NfcTagClientLockFunc callback = data->callback;

        data->callback = NULL;
        callback(&tag->pub, tag->lock, NULL, data->user_data);
    }
    return G_SOURCE_REMOVE;
}

static
void
nfc_tag_client_lock_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcTagClientObject* tag = THIS(user_data);
    GError* error = NULL;

    if (org_sailfishos_nfc_tag_call_release_finish(ORG_SAILFISHOS_NFC_TAG(
        proxy), result, &error)) {
        GDEBUG("Released %s lock", tag->name);
    } else {
        if (!tag->pub.present && g_error_matches(error, G_DBUS_ERROR,
            G_DBUS_ERROR_UNKNOWN_METHOD)) {
            /* This is OK, lock is often released after the tag is gone */
            GDEBUG("Couldn't release %s lock, tag is already gone", tag->name);
        } else {
            /* This is not expected (although probably still ok) */
            GWARN("Failed to release %s lock: %s", tag->name, GERRMSG(error));
        }
        g_error_free(error);
    }
    g_object_unref(tag);
}

static
void
nfc_tag_client_lock_acquire_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcTagClientLockData* data = user_data;
    NfcTagClientObject* tag = data->tag;
    GError* error = NULL;

    if (org_sailfishos_nfc_tag_call_acquire_finish(ORG_SAILFISHOS_NFC_TAG(
        proxy), result, &error)) {
        GDEBUG("Acquired %s lock", tag->name);
    } else {
        GWARN("Failed to acquire %s lock: %s", tag->name, GERRMSG(error));
    }

    if (data->callback) {
        NfcTagClientLockFunc callback = data->callback;

        data->callback = NULL;
        if (tag->lock) {
            /* No need to allocate a new lock */
            callback(&tag->pub, tag->lock, NULL, data->user_data);
            if (!error) {
                /* Release the extra lock */
                org_sailfishos_nfc_tag_call_release(tag->proxy, NULL,
                    nfc_tag_client_lock_done, g_object_ref(tag));
            }
        } else {
            NfcTagClientLock* lock;

            if (error) {
                lock = NULL;
            } else {
                /* Allocate the lock */
                lock = g_slice_new0(NfcTagClientLock);
                g_atomic_int_set(&lock->ref_count, 1);
                g_object_ref(lock->tag = tag);
                tag->lock = lock;
            }
            callback(&tag->pub, lock, error, data->user_data);
            nfc_tag_client_lock_unref(lock);
        }
    } else if (!error) {
        org_sailfishos_nfc_tag_call_release(tag->proxy, NULL,
            nfc_tag_client_lock_done, g_object_ref(tag));
    }

    if (error) {
        g_error_free(error);
    }
    nfc_tag_client_lock_data_deinit(data);
    gutil_slice_free(data);
}

NfcTagClientLock*
nfc_tag_client_lock_ref(
    NfcTagClientLock* lock)
{
    if (G_LIKELY(lock)) {
        g_atomic_int_inc(&lock->ref_count);
    }
    return lock;
}

void
nfc_tag_client_lock_unref(
    NfcTagClientLock* lock)
{
    if (G_LIKELY(lock) && g_atomic_int_dec_and_test(&lock->ref_count)) {
        NfcTagClientObject* tag = lock->tag;

        if (tag->proxy) {
            org_sailfishos_nfc_tag_call_release(tag->proxy, NULL,
                nfc_tag_client_lock_done, g_object_ref(tag));
        }
        if (tag->lock == lock) {
            tag->lock = NULL;
        }
        g_object_unref(tag);
        gutil_slice_free(lock);
    }
}

/*==========================================================================*
 * Implementation
 *==========================================================================*/

static inline
NfcTagClientObject*
nfc_tag_client_object_cast(
    NfcTagClient* pub)
{
    return G_LIKELY(pub) ?
        THIS(G_CAST(pub, NfcTagClientObject, pub)) :
        NULL;
}

static
int
nfc_tag_client_poll_param_key(
    const char* key)
{
    if (key) {
        if (!strcmp(key, "SEL_RES")) {
            return NFC_TAG_POLL_PARAM_SELRES;
        } else if (!strcmp(key, "NFCID1")) {
            return NFC_TAG_POLL_PARAM_NFCID1;
        } else if (!strcmp(key, "NFCID0")) {
            return NFC_TAG_POLL_PARAM_NFCID0;
        } else if (!strcmp(key, "APPDATA")) {
            return NFC_TAG_POLL_PARAM_APPDATA;
        } else if (!strcmp(key, "PROTINFO")) {
            return NFC_TAG_POLL_PARAM_PROTINFO;
        }
    }
    return -1;
}

static
void
nfc_tag_client_call_cancelled(
    GCancellable* cancel,
    NfcTagClientCallData* data)
{
    data->callback = NULL;
}

static
void
nfc_tag_client_call_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcTagClientCallData* call = user_data;
    NfcTagClientObject* self = call->obj;
    GError* error = NULL;

    if (!call->finish(ORG_SAILFISHOS_NFC_TAG(proxy), result, &error)) {
        GWARN("%s: %s", self->name, GERRMSG(error));
    }
    if (call->callback) {
        NfcTagClientCallFunc callback = call->callback;

        call->callback = NULL;
        callback(&self->pub, error, call->user_data);
    }
    if (error) {
        g_error_free(error);
    }
    if (call->cancel) {
        g_signal_handler_disconnect(call->cancel, call->cancel_id);
        g_object_unref(call->cancel);
    }
    if (call->destroy) {
        call->destroy(call->user_data);
    }
    g_object_unref(self);
    gutil_slice_free(call);
}

static
void
nfc_tag_client_update_valid_and_present(
    NfcTagClientObject* self)
{
    NfcTagClient* pub = &self->pub;
    NfcAdapterClient* adapter = self->adapter;
    gboolean valid, present;

    if (!adapter->valid || self->proxy_initializing) {
        valid = FALSE;
        present = FALSE;
    } else {
        valid = TRUE;
        present = (self->proxy && adapter->present &&
            gutil_strv_contains(adapter->tags, pub->path));
    }
    if (pub->valid != valid) {
        pub->valid = valid;
        nfc_tag_client_queue_signal(self, VALID);
    }
    if (pub->present != present) {
        pub->present = present;
        nfc_tag_client_queue_signal(self, PRESENT);
    }
}

static
void
nfc_tag_client_drop_proxy(
    NfcTagClientObject* self)
{
    NfcTagClient* pub = &self->pub;

    GASSERT(!self->proxy_initializing);
    if (self->proxy) {
        g_object_unref(self->proxy);
        self->proxy = NULL;
    }
    if (pub->valid) {
        pub->valid = FALSE;
        nfc_tag_client_queue_signal(self, VALID);
    }
    if (pub->present) {
        pub->present = FALSE;
        nfc_tag_client_queue_signal(self, PRESENT);
    }
    if (pub->interfaces[0]) {
        g_strfreev(self->interfaces);
        self->interfaces = NULL;
        pub->interfaces = &nfc_tag_client_empty_strv;
        nfc_tag_client_queue_signal(self, INTERFACES);
    }
    if (pub->ndef_records[0]) {
        g_strfreev(self->ndef_records);
        self->ndef_records = NULL;
        pub->ndef_records = &nfc_tag_client_empty_strv;
        nfc_tag_client_queue_signal(self, NDEF_RECORDS);
    }
}

static
void
nfc_tag_client_update(
    NfcTagClientObject* self)
{
    NfcTagClient* pub = &self->pub;

    if (gutil_strv_contains(self->adapter->tags, pub->path)) {
        if (!self->proxy && !self->proxy_initializing) {
            nfc_tag_client_reinit(self);
        }
    } else if (!self->proxy_initializing) {
        nfc_tag_client_drop_proxy(self);
    }
    nfc_tag_client_update_valid_and_present(self);
}

static
void
nfc_tag_client_adapter_changed(
    NfcAdapterClient* adapter,
    NFC_ADAPTER_PROPERTY property,
    void* user_data)
{
    NfcTagClientObject* self = THIS(user_data);

    nfc_tag_client_update(self);
    nfc_tag_client_emit_queued_signals(self);
}

static
void
nfc_tag_client_init_finished(
    NfcTagClientObject* self,
    gboolean present,
    gchar** interfaces,
    gchar** ndef_records,
    GVariant* dict)
{
    NfcTagClient* tag = &self->pub;

    if (tag->present != present) {
        tag->present = present;
        nfc_tag_client_queue_signal(self, PRESENT);
    }
    if (gutil_strv_equal(self->interfaces, interfaces)) {
        g_strfreev(interfaces);
    } else {
        g_strfreev(self->interfaces);
        tag->interfaces = self->interfaces = interfaces;
        nfc_tag_client_queue_signal(self, INTERFACES);
    }
    if (gutil_strv_equal(self->ndef_records, ndef_records)) {
        g_strfreev(ndef_records);
    } else {
        g_strfreev(self->ndef_records);
        tag->ndef_records = self->ndef_records = ndef_records;
        nfc_tag_client_queue_signal(self, NDEF_RECORDS);
    }
    if (dict) {
        GDEBUG("%s: Poll parameters", self->name);
        self->poll_params = nfc_parse_dict(self->poll_params, dict,
            nfc_tag_client_poll_param_key);
        g_variant_unref(dict);
    }
}

static
void
nfc_tag_client_init_5(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcTagClientObject* self = THIS(user_data);
    GError* error = NULL;
    gboolean present;
    gchar** interfaces;
    gchar** ndef_records;
    GVariant* dict;

    GASSERT(self->proxy_initializing);
    self->proxy_initializing = FALSE;
    if (org_sailfishos_nfc_tag_call_get_all3_finish(self->proxy,
        NULL, &present, NULL, NULL, NULL, &interfaces,
        &ndef_records, &dict, result, &error)) {
        nfc_tag_client_init_finished(self, present, interfaces,
            ndef_records, dict);
        nfc_tag_client_update_valid_and_present(self);
    } else {
        GERR("%s", GERRMSG(error));
        g_error_free(error);
        nfc_tag_client_drop_proxy(self);
    }
    nfc_tag_client_emit_queued_signals(self);
    g_object_unref(self);
}

static
void
nfc_tag_client_init_4(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcTagClientObject* self = THIS(user_data);
    GError* error = NULL;
    gboolean present;
    gchar** interfaces;
    gchar** ndef_records;

    GASSERT(self->proxy_initializing);
    if (!org_sailfishos_nfc_tag_call_get_all_finish(self->proxy,
        &self->version, &present, NULL, NULL, NULL, &interfaces,
        &ndef_records, result, &error)) {
        GERR("%s", GERRMSG(error));
        self->proxy_initializing = FALSE;
        g_error_free(error);
        nfc_tag_client_drop_proxy(self);
    } else if (self->version >= 3) {
        g_strfreev(interfaces);
        g_strfreev(ndef_records);
        org_sailfishos_nfc_tag_call_get_all3(self->proxy, NULL,
            nfc_tag_client_init_5, g_object_ref(self));
    } else {
        self->proxy_initializing = FALSE;
        nfc_tag_client_init_finished(self, present, interfaces,
            ndef_records, NULL);
        nfc_tag_client_update_valid_and_present(self);
        nfc_tag_client_emit_queued_signals(self);
    }
    g_object_unref(self);
}

static
void
nfc_tag_client_init_3(
    GObject* connection,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcTagClientObject* self = THIS(user_data);
    GError* error = NULL;

    GASSERT(!self->proxy);
    GASSERT(self->proxy_initializing);
    self->proxy = org_sailfishos_nfc_tag_proxy_new_finish(result, &error);
    if (self->proxy) {
        org_sailfishos_nfc_tag_call_get_all(self->proxy, NULL,
            nfc_tag_client_init_4, g_object_ref(self));
    } else {
        GERR("%s", GERRMSG(error));
        g_error_free(error);
        self->proxy_initializing = FALSE;
        nfc_tag_client_update_valid_and_present(self);
        nfc_tag_client_emit_queued_signals(self);
    }
    g_object_unref(self);
}

static
void
nfc_tag_client_init_2(
    NfcTagClientObject* self)
{
    org_sailfishos_nfc_tag_proxy_new(self->connection,
        G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, NFCD_DBUS_DAEMON_NAME,
        self->pub.path, NULL, nfc_tag_client_init_3, g_object_ref(self));
}

static
void
nfc_tag_client_init_1(
    GObject* object,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcTagClientObject* self = THIS(user_data);
    GError* error = NULL;

    GASSERT(self->proxy_initializing);
    self->connection = g_bus_get_finish(result, &error);
    if (self->connection) {
        GDEBUG("Bus connected");
        nfc_tag_client_init_2(self);
    } else {
        GERR("Failed to attach to NFC daemon bus: %s", GERRMSG(error));
        g_error_free(error);
        self->proxy_initializing = FALSE;
        nfc_tag_client_update_valid_and_present(self);
        nfc_tag_client_emit_queued_signals(self);
    }
    g_object_unref(self);
}
        
static
void
nfc_tag_client_reinit(
    NfcTagClientObject* self)
{
    GASSERT(!self->proxy_initializing);
    self->proxy_initializing = TRUE;
    nfc_tag_client_init_2(self);
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

GDBusConnection*
nfc_tag_client_connection(
    NfcTagClient* tag)
{
    NfcTagClientObject* self = nfc_tag_client_object_cast(tag);

    return G_LIKELY(self) ? self->connection : NULL;
}

/*==========================================================================*
 * API
 *==========================================================================*/

NfcTagClient*
nfc_tag_client_new(
    const char* path)
{
    if (G_LIKELY(path) && g_variant_is_object_path(path)) {
        const char* sep = strrchr(path, '/');
        if (sep > path) {
            char* adapter_path = g_strndup(path, sep - path);
            NfcTagClientObject* obj = NULL;

            if (nfc_tag_client_table) {
                obj = g_hash_table_lookup(nfc_tag_client_table, path);
            }
            if (obj) {
                g_object_ref(obj);
            } else {
                char* key = g_strdup(path);

                GVERBOSE_("%s", path);
                obj = g_object_new(THIS_TYPE, NULL);
                if (!nfc_tag_client_table) {
                    nfc_tag_client_table = g_hash_table_new_full(g_str_hash,
                        g_str_equal, g_free, NULL);
                }
                g_hash_table_insert(nfc_tag_client_table, key, obj);
                obj->pub.path = key;
                obj->name = key + ((sep - path) + 1);
                obj->adapter = nfc_adapter_client_new(adapter_path);
                obj->adapter_event_id[ADAPTER_VALID_CHANGED] =
                    nfc_adapter_client_add_property_handler(obj->adapter,
                        NFC_ADAPTER_PROPERTY_VALID,
                        nfc_tag_client_adapter_changed, obj);
                obj->adapter_event_id[ADAPTER_TAGS_CHANGED] =
                    nfc_adapter_client_add_property_handler(obj->adapter,
                        NFC_ADAPTER_PROPERTY_TAGS,
                        nfc_tag_client_adapter_changed, obj);
                obj->connection = nfc_adapter_client_connection(obj->adapter);
                nfc_tag_client_update(obj);
                if (obj->connection) {
                    /* Already attached to the bus */
                    g_object_ref(obj->connection);
                    nfc_tag_client_init_2(obj);
                } else {
                    g_bus_get(NFCD_DBUS_TYPE, NULL, nfc_tag_client_init_1,
                        g_object_ref(obj));
                }
            }
            g_free(adapter_path);
            return &obj->pub;
        }
    }
    return NULL;
}

NfcTagClient*
nfc_tag_client_ref(
    NfcTagClient* tag)
{
    if (G_LIKELY(tag)) {
        g_object_ref(nfc_tag_client_object_cast(tag));
        return tag;
    } else {
        return NULL;
    }
}

void
nfc_tag_client_unref(
    NfcTagClient* tag)
{
    if (G_LIKELY(tag)) {
        g_object_unref(nfc_tag_client_object_cast(tag));
    }
}

NfcTagClientLock*
nfc_tag_client_get_lock(
    NfcTagClient* tag) /* Since 1.0.10 */
{
    return G_LIKELY(tag) ? nfc_tag_client_object_cast(tag)->lock : NULL;
}

gboolean
nfc_tag_client_acquire_lock(
    NfcTagClient* tag,
    gboolean wait,
    GCancellable* cancel,
    NfcTagClientLockFunc callback,
    void* user_data,
    GDestroyNotify destroy)
{
    NfcTagClientObject* self = nfc_tag_client_object_cast(tag);

    if (self && tag->valid && tag->present && (callback || destroy) &&
        (!cancel || !g_cancellable_is_cancelled(cancel))) {
        if (self->lock) {
            NfcTagClientLockDataIdle* idle =
                g_slice_new0(NfcTagClientLockDataIdle);

            /* Just invoke completion on a fresh stack */
            nfc_tag_client_lock_data_init(&idle->data, self, callback, destroy,
                user_data, cancel);
            nfc_tag_client_lock_ref(self->lock);
            idle->id = g_idle_add_full(G_PRIORITY_DEFAULT,
                nfc_tag_client_lock_idle_callback, idle,
                nfc_tag_client_lock_data_idle_free);
        } else {
            NfcTagClientLockData* call = g_slice_new0(NfcTagClientLockData);

            nfc_tag_client_lock_data_init(call, self, callback, destroy,
                user_data, cancel);
            
            /*
             * We still let the call to complete normally even if it gets
             * cancelled (by not passing GCacellable through), to maintain
             * the lock reference count.
             */
            org_sailfishos_nfc_tag_call_acquire(self->proxy, wait, NULL,
                nfc_tag_client_lock_acquire_done, call);
        }
        return TRUE;
    } else {
        /* Destroy callback is always invoked even if we return FALSE */
        if (destroy) {
            destroy(user_data);
        }
        return FALSE;
    }
}

const GUtilData*
nfc_tag_client_poll_param(
    NfcTagClient* tag,
    NFC_TAG_POLL_PARAM param) /* Since 1.0.10 */
{
    NfcTagClientObject* self = nfc_tag_client_object_cast(tag);

    return (self && self->poll_params) ?
        g_hash_table_lookup(self->poll_params, GINT_TO_POINTER(param)) :
        NULL;
}

gboolean
nfc_tag_client_deactivate(
    NfcTagClient* tag,
    GCancellable* cancel,
    NfcTagClientCallFunc callback,
    void* user_data,
    GDestroyNotify destroy) /* Since 1.0.9 */
{
    NfcTagClientObject* self = nfc_tag_client_object_cast(tag);

    if (self && tag->valid && tag->present && (!cancel ||
        !g_cancellable_is_cancelled(cancel))) {
        if (callback || destroy) {
            NfcTagClientCallData* call = g_slice_new0(NfcTagClientCallData);

            g_object_ref(call->obj = self);
            call->callback = callback;
            call->destroy = destroy;
            call->user_data = user_data;
            call->finish = org_sailfishos_nfc_tag_call_deactivate_finish;
            if (cancel) {
                g_object_ref(call->cancel = cancel);
                call->cancel_id = g_cancellable_connect(cancel,
                    G_CALLBACK(nfc_tag_client_call_cancelled), call, NULL);
            }
            org_sailfishos_nfc_tag_call_deactivate(self->proxy, cancel,
                nfc_tag_client_call_done, call);
        } else {
            /* No need to allocate the context */
            org_sailfishos_nfc_tag_call_deactivate(self->proxy, NULL, NULL,
                NULL);
        }
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
nfc_tag_client_add_property_handler(
    NfcTagClient* tag,
    NFC_TAG_PROPERTY property,
    NfcTagPropertyFunc callback,
    void* user_data)
{
    NfcTagClientObject* self = nfc_tag_client_object_cast(tag);

    return G_LIKELY(self) ? nfc_client_base_add_property_handler(&self->base,
        property, (NfcClientBasePropertyFunc) callback, user_data) : 0;
}

void
nfc_tag_client_remove_handler(
    NfcTagClient* tag,
    gulong id)
{
    if (G_LIKELY(id)) {
        NfcTagClientObject* self = nfc_tag_client_object_cast(tag);

        if (G_LIKELY(self)) {
            g_signal_handler_disconnect(self, id);
        }
    }
}

void
nfc_tag_client_remove_handlers(
    NfcTagClient* tag,
    gulong* ids,
    guint n)
{
    gutil_disconnect_handlers(nfc_tag_client_object_cast(tag), ids, n);
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
nfc_tag_client_object_init(
    NfcTagClientObject* self)
{
    NfcTagClient* pub = &self->pub;

    pub->interfaces = &nfc_tag_client_empty_strv;
    pub->ndef_records = &nfc_tag_client_empty_strv;
    self->proxy_initializing = TRUE;
}

static
void
nfc_tag_client_object_finalize(
    GObject* object)
{
    NfcTagClientObject* self = THIS(object);
    NfcTagClient* pub = &self->pub;

    GVERBOSE_("%s", pub->path);
    GASSERT(!self->lock); /* Lock holds a reference to the tag */
    nfc_tag_client_drop_proxy(self);
    nfc_adapter_client_remove_all_handlers(self->adapter,
        self->adapter_event_id);
    nfc_adapter_client_unref(self->adapter);
    if (self->connection) {
        g_object_unref(self->connection);
    }
    if (self->poll_params) {
        g_hash_table_destroy(self->poll_params);
    }
    g_strfreev(self->interfaces);
    g_strfreev(self->ndef_records);
    g_hash_table_remove(nfc_tag_client_table, pub->path);
    if (g_hash_table_size(nfc_tag_client_table) == 0) {
        g_hash_table_unref(nfc_tag_client_table);
        nfc_tag_client_table = NULL;
    }
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
nfc_tag_client_object_class_init(
    NfcTagClientObjectClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = nfc_tag_client_object_finalize;
    klass->public_offset = G_STRUCT_OFFSET(NfcTagClientObject, pub);
    klass->valid_offset = G_STRUCT_OFFSET(NfcTagClientObject, pub.valid);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
