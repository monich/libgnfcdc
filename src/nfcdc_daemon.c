/*
 * Copyright (C) 2019-2024 Slava Monich <slava@monich.com>
 * Copyright (C) 2019-2022 Jolla Ltd.
 *
 * You may use this file under the terms of the BSD license as follows:
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *
 *  2. Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer
 *     in the documentation and/or other materials provided with the
 *     distribution.
 *
 *  3. Neither the names of the copyright holders nor the names of its
 *     contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
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

#include "nfcdc_base.h"
#include "nfcdc_dbus.h"
#include "nfcdc_daemon_p.h"
#include "nfcdc_log.h"
#include "nfcdc_peer_service_p.h"

#include <gutil_macros.h>
#include <gutil_misc.h>
#include <gutil_strv.h>

#include "org.sailfishos.nfc.Daemon.h"
#include "org.sailfishos.nfc.Settings.h"

/*
 * NFCDC_NEED_PEER_SERVICE=0 can be used to avoid compiling in
 * nfc_peer_service.c and org.sailfishos.nfc.LocalService.c
 */
#ifndef NFCDC_NEED_PEER_SERVICE
# define NFCDC_NEED_PEER_SERVICE 1
#endif /* NFCDC_NEED_PEER_SERVICE */

typedef NfcClientBaseClass NfcDaemonClientObjectClass;
typedef struct nfc_daemon_client_object {
    NfcClientBase base;
    NfcDaemonClient pub;
    GStrV* adapters;
    GError* daemon_error;
    GDBusConnection* connection;

    /* Daemon interface */
    OrgSailfishosNfcDaemon* proxy;
    gulong adapters_changed_id;
    gboolean daemon_watch_initializing;
    gboolean daemon_present;
    guint daemon_watch_id;
    GError* error;

    /* Settings interface */
    OrgSailfishosNfcSettings* settings;
    gulong settings_enabled_changed_id;
    gboolean settings_watch_initializing;
    gboolean settings_present;
    guint settings_watch_id;
    GError* settings_error;
} NfcDaemonClientObject;

typedef
gboolean
(*NfcDaemonClientCallFinishFunc)(
    OrgSailfishosNfcDaemon* proxy,
    GAsyncResult* res,
    GError** error);

typedef struct nfc_daemon_client_call_data {
    NfcDaemonClientObject* obj;
    NfcDaemonClientCallFunc callback;
    NfcDaemonClientCallFinishFunc finish;
    GDestroyNotify destroy;
    void* user_data;
    GCancellable* cancel;
    gulong cancel_id;
} NfcDaemonClientCallData;

typedef struct nfc_request_type {
    const char* Name;
    const char* name;
    void (*call_request)(
        OrgSailfishosNfcDaemon* proxy,
        guint on,
        guint off,
        GCancellable* cancel,
        GAsyncReadyCallback callback,
        gpointer user_data);
    gboolean (*finish_request)(
        OrgSailfishosNfcDaemon* proxy,
        guint* id,
        GAsyncResult* res,
        GError** error);
    void (*call_release)(
        OrgSailfishosNfcDaemon* proxy,
        guint id,
        GCancellable* cancel,
        GAsyncReadyCallback callback,
        gpointer user_data);
    gboolean (*finish_release)(
        OrgSailfishosNfcDaemon* proxy,
        GAsyncResult* res,
        GError** error);
} NfcRequestType;

typedef struct nfc_request_impl {
    gint refcount;
    guint on;
    guint off;
    NfcDaemonClientObject* daemon;
    gulong present_id;
    gboolean pending;
    gboolean cancelled;
    guint id;
    const NfcRequestType* type;
} NfcRequestImpl;

typedef struct nfc_mode_request_priv {
    NfcModeRequest pub;
    NfcRequestImpl* impl;
} NfcModeRequestPriv;

typedef struct nfc_tech_request_priv {
    NfcTechRequest pub;
    NfcRequestImpl* impl;
} NfcTechRequestPriv;

#define PARENT_CLASS nfc_daemon_client_object_parent_class
#define THIS_TYPE nfc_daemon_client_object_get_type()
#define THIS(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, THIS_TYPE, \
    NfcDaemonClientObject)

GType THIS_TYPE G_GNUC_INTERNAL;
G_DEFINE_TYPE(NfcDaemonClientObject, nfc_daemon_client_object, \
    NFC_CLIENT_TYPE_BASE)

NFC_CLIENT_BASE_ASSERT_VALID(NFC_DAEMON_PROPERTY_VALID);
NFC_CLIENT_BASE_ASSERT_COUNT(NFC_DAEMON_PROPERTY_COUNT);

#define SIGNAL_BIT_(x) NFC_CLIENT_BASE_SIGNAL_BIT(NFC_DAEMON_PROPERTY_##x)

#define nfc_daemon_client_emit_queued_signals(self) \
    nfc_client_base_emit_queued_signals(&(self)->base)
#define nfc_daemon_client_queue_signal(self,property) \
    ((self)->base.queued_signals |= NFC_CLIENT_BASE_SIGNAL_BIT(property))
#define nfc_daemon_client_queue_signal_(self,NAME) \
    ((self)->base.queued_signals |= SIGNAL_BIT_(NAME))

static char* nfc_daemon_client_empty_strv = NULL;
static NfcDaemonClientObject* nfc_daemon_client_instance = NULL;

/*==========================================================================*
 * Implementation
 *==========================================================================*/

static inline
NfcDaemonClientObject*
nfc_daemon_client_object_cast(
    NfcDaemonClient* pub)
{
    return G_LIKELY(pub) ?
        THIS(G_CAST(pub, NfcDaemonClientObject, pub)) :
        NULL;
}

static
void
nfc_daemon_client_call_cancelled(
    GCancellable* cancel,
    NfcDaemonClientCallData* data)
{
    data->callback = NULL;
}

static
NfcDaemonClientCallData*
nfc_daemon_client_call_new(
    NfcDaemonClientObject* self,
    NfcDaemonClientCallFinishFunc finish,
    GCancellable* cancel,
    NfcDaemonClientCallFunc callback,
    void* user_data,
    GDestroyNotify destroy)
{
    NfcDaemonClientCallData* call = g_slice_new0(NfcDaemonClientCallData);

    g_object_ref(call->obj = self);
    call->callback = callback;
    call->destroy = destroy;
    call->user_data = user_data;
    call->finish = finish;
    if (cancel) {
        g_object_ref(call->cancel = cancel);
        call->cancel_id = g_cancellable_connect(cancel,
            G_CALLBACK(nfc_daemon_client_call_cancelled), call, NULL);
    }
    return call;
}

static
void
nfc_daemon_client_call_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcDaemonClientCallData* call = user_data;
    NfcDaemonClientObject* self = call->obj;
    GError* error = NULL;

    if (!call->finish(ORG_SAILFISHOS_NFC_DAEMON(proxy), result, &error)) {
        GWARN("%s", GERRMSG(error));
    }
    if (call->callback) {
        NfcDaemonClientCallFunc callback = call->callback;

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
nfc_daemon_client_update_valid_and_present(
    NfcDaemonClientObject* self)
{
    NfcDaemonClient* pub = &self->pub;
    gboolean valid, present;

    if (self->daemon_watch_initializing || self->settings_watch_initializing) {
        valid = FALSE;
        present = FALSE;
    } else if (pub->error || !self->daemon_present || !self->settings_present) {
        valid = TRUE;
        present = FALSE;
    } else if (self->proxy && self->settings) {
        valid = TRUE;
        present = TRUE;
    } else {
        /* Asynchronous initialization is in progress */
        valid = FALSE;
        present = FALSE;
    }
    if (pub->valid != valid) {
        pub->valid = valid;
        nfc_daemon_client_queue_signal_(self, VALID);
    }
    if (pub->present != present) {
        pub->present = present;
        nfc_daemon_client_queue_signal_(self, PRESENT);
    }
}

static
void
nfc_daemon_client_update_public_error(
    NfcDaemonClientObject* self)
{
    NfcDaemonClient* pub = &self->pub;
    GError* error = self->daemon_error ? self->daemon_error :
        self->settings_error;

    if (pub->error != error) {
        pub->error = error;
        nfc_daemon_client_queue_signal_(self, ERROR);
    }
}

static
void
nfc_daemon_client_set_daemon_error(
    NfcDaemonClientObject* self,
    GError* error)
{
    if (!self->daemon_error || !error) {
        if (self->daemon_error) {
            g_error_free(self->daemon_error);
        }
        self->daemon_error = error;
        nfc_daemon_client_update_public_error(self);
    } else if (error) {
        g_error_free(error);
    }
}

static
void
nfc_daemon_client_set_settings_error(
    NfcDaemonClientObject* self,
    GError* error)
{
    if (!self->settings_error || !error) {
        if (self->settings_error) {
            g_error_free(self->settings_error);
        }
        self->settings_error = error;
        nfc_daemon_client_update_public_error(self);
    } else if (error) {
        g_error_free(error);
    }
}

static
void
nfc_daemon_client_daemon_adapters_changed(
    OrgSailfishosNfcDaemon* proxy,
    GStrV* adapters,
    NfcDaemonClientObject* self)
{
    /* We may receive a signal before the initial query has completed */
    GASSERT(!self->proxy || self->proxy == proxy);
    if (!gutil_strv_equal(self->adapters, adapters)) {
        NfcDaemonClient* pub = &self->pub;

        DUMP_STRV(NULL, "Adapters", "=", adapters);
        g_strfreev(self->adapters);
        pub->adapters = (self->adapters = g_strdupv(adapters)) ?
            self->adapters : &nfc_daemon_client_empty_strv;
        nfc_daemon_client_queue_signal_(self, ADAPTERS);
        nfc_daemon_client_emit_queued_signals(self);
    }
}

static
void
nfc_daemon_client_daemon_set_version(
    NfcDaemonClientObject* self,
    int version)
{
    NfcDaemonClient* pub = &self->pub;

    GDEBUG("NFC daemon version %d.%d.%d",
        NFC_DAEMON_VERSION_MAJOR(version),
        NFC_DAEMON_VERSION_MINOR(version),
        NFC_DAEMON_VERSION_NANO(version));
    if (pub->version != version) {
        pub->version = version;
        nfc_daemon_client_queue_signal_(self, VERSION);
    }
}

static
void
nfc_daemon_client_daemon_set_adapters(
    NfcDaemonClientObject* self,
    char** adapters)
{
    NfcDaemonClient* pub = &self->pub;

    if (!gutil_strv_equal(self->adapters, adapters)) {
        g_strfreev(self->adapters);
        pub->adapters = (self->adapters = adapters) ?
            self->adapters : &nfc_daemon_client_empty_strv;
        nfc_daemon_client_queue_signal_(self, ADAPTERS);
    } else {
        g_strfreev(adapters);
    }
}

static
void
nfc_daemon_client_daemon_set_mode(
    NfcDaemonClientObject* self,
    NFC_MODE mode)
{
    NfcDaemonClient* pub = &self->pub;

    GDEBUG("NFC mode %02x", mode);
    if (pub->mode != mode) {
        pub->mode = mode;
        nfc_daemon_client_queue_signal_(self, MODE);
    }
}

static
void
nfc_daemon_client_daemon_set_techs(
    NfcDaemonClientObject* self,
    NFC_TECH techs)
{
    NfcDaemonClient* pub = &self->pub;

    GDEBUG("NFC techs %02x", techs);
    if (pub->techs != techs) {
        pub->techs = techs;
        nfc_daemon_client_queue_signal_(self, TECHS);
    }
}

#if NFCDC_NEED_PEER_SERVICE

static
void
nfc_daemon_client_register_service_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcPeerService* service = user_data;
    OrgSailfishosNfcDaemon* daemon = ORG_SAILFISHOS_NFC_DAEMON(proxy);
    GError* error = NULL;
    guint sap;

    if (org_sailfishos_nfc_daemon_call_register_local_service_finish(daemon,
        &sap, result, &error)) {
        nfc_peer_service_registered(service, sap);
    } else {
        nfc_peer_service_registeration_failed(service, error);
        g_error_free(error);
    }
    nfc_peer_service_unref(service);
}

#endif /* NFCDC_NEED_PEER_SERVICE */

static
void
nfc_daemon_client_daemon_get_all4_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcDaemonClientObject* self = THIS(user_data);
    OrgSailfishosNfcDaemon* daemon = ORG_SAILFISHOS_NFC_DAEMON(proxy);
    GError* error = NULL;
    gint iface_version = 0;
    char** adapters = NULL;
    gint version = 0;
    guint mode = 0, techs = 0;

    GASSERT(!self->proxy);
    GASSERT(self->adapters_changed_id);
    if (org_sailfishos_nfc_daemon_call_get_all4_finish(daemon,
        &iface_version, &adapters, &version, &mode, &techs, result, &error)) {
        GASSERT(iface_version >= 3);
        nfc_daemon_client_daemon_set_version(self, version);
        nfc_daemon_client_daemon_set_adapters(self, adapters);
        nfc_daemon_client_daemon_set_mode(self, mode);
        nfc_daemon_client_daemon_set_techs(self, techs);
        self->proxy = daemon;
    } else {
        GERR("Failed to talk to NFC daemon: %s", GERRMSG(error));
        nfc_daemon_client_set_daemon_error(self, error);
        gutil_disconnect_handlers(daemon, &self->adapters_changed_id, 1);
        g_object_unref(daemon);
    }
    nfc_daemon_client_update_valid_and_present(self);
    nfc_daemon_client_emit_queued_signals(self);
    g_object_unref(self);
}

static
void
nfc_daemon_client_daemon_get_all3_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcDaemonClientObject* self = THIS(user_data);
    OrgSailfishosNfcDaemon* daemon = ORG_SAILFISHOS_NFC_DAEMON(proxy);
    GError* error = NULL;
    gint iface_version = 0;
    char** adapters = NULL;
    gint version = 0;
    guint mode = 0;

    GASSERT(!self->proxy);
    GASSERT(self->adapters_changed_id);
    if (org_sailfishos_nfc_daemon_call_get_all3_finish(daemon,
        &iface_version, &adapters, &version, &mode, result, &error)) {
        GASSERT(iface_version >= 3);
        nfc_daemon_client_daemon_set_version(self, version);
        nfc_daemon_client_daemon_set_adapters(self, adapters);
        nfc_daemon_client_daemon_set_mode(self, mode);
        self->proxy = daemon;
    } else {
        GERR("Failed to talk to NFC daemon: %s", GERRMSG(error));
        nfc_daemon_client_set_daemon_error(self, error);
        gutil_disconnect_handlers(daemon, &self->adapters_changed_id, 1);
        g_object_unref(daemon);
    }
    nfc_daemon_client_update_valid_and_present(self);
    nfc_daemon_client_emit_queued_signals(self);
    g_object_unref(self);
}

static
void
nfc_daemon_client_daemon_get_all2_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcDaemonClientObject* self = THIS(user_data);
    OrgSailfishosNfcDaemon* daemon = ORG_SAILFISHOS_NFC_DAEMON(proxy);
    GError* error = NULL;
    gint iface_version = 0;
    char** adapters = NULL;
    gint version = 0;

    GASSERT(!self->proxy);
    GASSERT(self->adapters_changed_id);
    if (org_sailfishos_nfc_daemon_call_get_all2_finish(daemon,
        &iface_version, &adapters, &version, result, &error)) {
        GASSERT(iface_version == 2);
        nfc_daemon_client_daemon_set_version(self, version);
        nfc_daemon_client_daemon_set_adapters(self, adapters);
        self->proxy = daemon;
    } else {
        GERR("Failed to talk to NFC daemon: %s", GERRMSG(error));
        nfc_daemon_client_set_daemon_error(self, error);
        gutil_disconnect_handlers(daemon, &self->adapters_changed_id, 1);
        g_object_unref(daemon);
    }
    nfc_daemon_client_update_valid_and_present(self);
    nfc_daemon_client_emit_queued_signals(self);
    g_object_unref(self);
}

static
void
nfc_daemon_client_daemon_get_all_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcDaemonClientObject* self = THIS(user_data);
    OrgSailfishosNfcDaemon* daemon = ORG_SAILFISHOS_NFC_DAEMON(proxy);
    GError* error = NULL;
    gint iface_version = 0;
    char** adapters = NULL;

    GASSERT(!self->proxy);
    GASSERT(self->adapters_changed_id);
    if (org_sailfishos_nfc_daemon_call_get_all_finish(daemon,
        &iface_version, &adapters, result, &error)) {
        NfcDaemonClient* pub = &self->pub;

        GDEBUG("NFC daemon interface version %d", iface_version);
        nfc_daemon_client_daemon_set_adapters(self, adapters);
        if (iface_version >= 4) {
            org_sailfishos_nfc_daemon_call_get_all4(daemon, NULL,
                nfc_daemon_client_daemon_get_all4_done, g_object_ref(self));
        } else if (iface_version == 3) {
            org_sailfishos_nfc_daemon_call_get_all3(daemon, NULL,
                nfc_daemon_client_daemon_get_all3_done, g_object_ref(self));
        } else if (iface_version == 2) {
            org_sailfishos_nfc_daemon_call_get_all2(daemon, NULL,
                nfc_daemon_client_daemon_get_all2_done, g_object_ref(self));
        } else {
            if (pub->version) {
                pub->version = 0;
                nfc_daemon_client_queue_signal_(self, VERSION);
            }
            self->proxy = daemon;
        }
    } else {
        GERR("Failed to talk to NFC daemon: %s", GERRMSG(error));
        nfc_daemon_client_set_daemon_error(self, error);
        gutil_disconnect_handlers(daemon, &self->adapters_changed_id, 1);
        g_object_unref(daemon);
    }
    nfc_daemon_client_update_valid_and_present(self);
    nfc_daemon_client_emit_queued_signals(self);
    g_object_unref(self);
}

static
void
nfc_daemon_client_new_daemon(
    GObject* object,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcDaemonClientObject* self = THIS(user_data);
    GError* error = NULL;
    OrgSailfishosNfcDaemon* daemon =
        org_sailfishos_nfc_daemon_proxy_new_finish(result, &error);

    if (daemon) {
        GDEBUG("Connected to NFC daemon");
        self->adapters_changed_id =
            g_signal_connect(daemon, "adapters-changed",
                G_CALLBACK(nfc_daemon_client_daemon_adapters_changed), self);
        org_sailfishos_nfc_daemon_call_get_all(daemon, NULL,
            nfc_daemon_client_daemon_get_all_done, g_object_ref(self));
    } else {
        GERR("Failed to attach to NFC daemon: %s", GERRMSG(error));
        nfc_daemon_client_set_daemon_error(self, error);
    }
    nfc_daemon_client_update_valid_and_present(self);
    nfc_daemon_client_emit_queued_signals(self);
    g_object_unref(self);
}

static
void
nfc_daemon_client_settings_enabled_changed(
    OrgSailfishosNfcSettings* proxy,
    gboolean enabled,
    NfcDaemonClientObject* self)
{
    NfcDaemonClient* pub = &self->pub;

    /* We may receive a signal before the initial query has completed */
    GASSERT(!self->settings || self->settings == proxy);
    if (pub->enabled != enabled) {
        pub->enabled = enabled;
        GDEBUG("NFC %sabled", enabled ? "en" : "dis");
        nfc_daemon_client_queue_signal_(self, ENABLED);
        nfc_daemon_client_emit_queued_signals(self);
    }
}

static
void
nfc_daemon_client_new_settings_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcDaemonClientObject* self = THIS(user_data);
    OrgSailfishosNfcSettings* settings = ORG_SAILFISHOS_NFC_SETTINGS(proxy);
    GError* error = NULL;
    gboolean enabled;

    GASSERT(!self->settings);
    GASSERT(self->settings_enabled_changed_id);
    if (org_sailfishos_nfc_settings_call_get_enabled_finish(settings,
        &enabled, result, &error)) {
        NfcDaemonClient* pub = &self->pub;

        if (pub->enabled != enabled) {
            pub->enabled = enabled;
            nfc_daemon_client_queue_signal_(self, ENABLED);
        }
        self->settings = settings;
    } else {
        GERR("Failed to query NFC settings: %s", GERRMSG(error));
        nfc_daemon_client_set_settings_error(self, error);
        gutil_disconnect_handlers(settings,
            &self->settings_enabled_changed_id, 1);
        g_object_unref(settings);
    }
    nfc_daemon_client_update_valid_and_present(self);
    nfc_daemon_client_emit_queued_signals(self);
    g_object_unref(self);
}

static
void
nfc_daemon_client_new_settings(
    GObject* object,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcDaemonClientObject* self = THIS(user_data);
    GError* error = NULL;
    OrgSailfishosNfcSettings* settings =
        org_sailfishos_nfc_settings_proxy_new_finish(result, &error);

    if (settings) {
        GDEBUG("Connected to NFC settings");
        self->settings_enabled_changed_id =
            g_signal_connect(settings, "enabled-changed",
                G_CALLBACK(nfc_daemon_client_settings_enabled_changed), self);
        org_sailfishos_nfc_settings_call_get_enabled(settings, NULL,
            nfc_daemon_client_new_settings_done, g_object_ref(self));
    } else {
        GERR("Failed to attach to NFC settings: %s", GERRMSG(error));
        nfc_daemon_client_set_settings_error(self, error);
    }
    nfc_daemon_client_update_valid_and_present(self);
    nfc_daemon_client_emit_queued_signals(self);
    g_object_unref(self);
}

static
void
nfc_daemon_client_drop_daemon_proxy(
    NfcDaemonClientObject* self)
{
    NfcDaemonClient* pub = &self->pub;

    if (self->proxy) {
        gutil_disconnect_handlers(self->proxy, &self->adapters_changed_id, 1);
        g_object_unref(self->proxy);
        self->proxy = NULL;
    }
    if (pub->valid) {
        pub->valid = FALSE;
        nfc_daemon_client_queue_signal_(self, VALID);
    }
    if (pub->present) {
        pub->present = FALSE;
        nfc_daemon_client_queue_signal_(self, PRESENT);
    }
    if (pub->adapters[0]) {
        g_strfreev(self->adapters);
        self->adapters = NULL;
        pub->adapters = &nfc_daemon_client_empty_strv;
        nfc_daemon_client_queue_signal_(self, ADAPTERS);
    }
    if (pub->version) {
        pub->version = 0;
        nfc_daemon_client_queue_signal_(self, VERSION);
    }
    if (pub->mode != NFC_MODE_NONE) {
        pub->mode = NFC_MODE_NONE;
        nfc_daemon_client_queue_signal_(self, MODE);
    }
}

static
void
nfc_daemon_client_drop_settings_proxy(
    NfcDaemonClientObject* self)
{
    NfcDaemonClient* pub = &self->pub;

    if (self->settings) {
        gutil_disconnect_handlers(self->settings,
            &self->settings_enabled_changed_id, 1);
        g_object_unref(self->settings);
        self->settings = NULL;
    }
    if (pub->enabled) {
        pub->enabled = FALSE;
        nfc_daemon_client_queue_signal_(self, ENABLED);
    }
}

static
void
nfc_daemon_client_daemon_appeared(
    GDBusConnection* connection,
    const gchar* name,
    const gchar* owner,
    gpointer user_data)
{
    NfcDaemonClientObject* self = THIS(user_data);

    GDEBUG("Name '%s' is owned by %s", name, owner);
    GASSERT(!self->proxy);
    GASSERT(!self->daemon_present);
    self->daemon_watch_initializing = FALSE;
    self->daemon_present = TRUE;
    org_sailfishos_nfc_daemon_proxy_new(self->connection,
        G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, NFCD_DBUS_DAEMON_NAME,
        NFCD_DBUS_DAEMON_PATH, NULL, nfc_daemon_client_new_daemon,
        g_object_ref(self));
    nfc_daemon_client_set_daemon_error(self, NULL);
    nfc_daemon_client_update_valid_and_present(self);
    nfc_daemon_client_emit_queued_signals(self);
}

static
void
nfc_daemon_client_daemon_vanished(
    GDBusConnection* connection,
    const gchar* name,
    gpointer user_data)
{
    NfcDaemonClientObject* self = THIS(user_data);

    if (self->daemon_present) {
        GDEBUG("Name '%s' has disappeared", name);
        self->daemon_present = FALSE;
    } else {
        GDEBUG("Name '%s' not found", name);
    }
    self->daemon_watch_initializing = FALSE;
    nfc_daemon_client_drop_daemon_proxy(self);
    nfc_daemon_client_update_valid_and_present(self);
    nfc_daemon_client_emit_queued_signals(self);
}

static
void
nfc_daemon_client_settings_appeared(
    GDBusConnection* connection,
    const gchar* name,
    const gchar* owner,
    gpointer user_data)
{
    NfcDaemonClientObject* self = THIS(user_data);

    GDEBUG("Name '%s' is owned by %s", name, owner);
    GASSERT(!self->settings);
    GASSERT(!self->settings_present);
    self->settings_watch_initializing = FALSE;
    self->settings_present = TRUE;
    org_sailfishos_nfc_settings_proxy_new(self->connection,
        G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, NFCD_DBUS_SETTINGS_NAME,
        NFCD_DBUS_SETTINGS_PATH, NULL, nfc_daemon_client_new_settings,
        g_object_ref(self));
    nfc_daemon_client_set_settings_error(self, NULL);
    nfc_daemon_client_update_valid_and_present(self);
    nfc_daemon_client_emit_queued_signals(self);
}

static
void
nfc_daemon_client_settings_vanished(
    GDBusConnection* connection,
    const gchar* name,
    gpointer user_data)
{
    NfcDaemonClientObject* self = THIS(user_data);

    if (self->settings_present) {
        GDEBUG("Name '%s' has disappeared", name);
        self->settings_present = FALSE;
    } else {
        GDEBUG("Name '%s' not found", name);
    }
    self->settings_watch_initializing = FALSE;
    nfc_daemon_client_drop_settings_proxy(self);
    nfc_daemon_client_update_valid_and_present(self);
    nfc_daemon_client_emit_queued_signals(self);
}

/*==========================================================================*
 * NfcRequestImpl
 *==========================================================================*/

static
NfcRequestImpl*
nfc_request_impl_ref(
    NfcRequestImpl* impl)
{
    GASSERT(impl->refcount > 0);
    g_atomic_int_inc(&impl->refcount);
    return impl;
}

static
void
nfc_request_impl_unref(
    NfcRequestImpl* impl)
{
    GASSERT(impl->refcount > 0);
    if (g_atomic_int_dec_and_test(&impl->refcount)) {
        g_signal_handler_disconnect(impl->daemon, impl->present_id);
        g_object_unref(impl->daemon);
        GASSERT(!impl->pending);
        gutil_slice_free(impl);
    }
}

static
void
nfc_request_impl_release_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcRequestImpl* impl = user_data;
    const NfcRequestType* type = impl->type;
    OrgSailfishosNfcDaemon* daemon = ORG_SAILFISHOS_NFC_DAEMON(proxy);
    GError* error = NULL;

    GASSERT(impl->pending);
    impl->pending = FALSE;

    if (type->finish_release(daemon, result, &error)) {
        GDEBUG("Dropped %s request %u", type->name, impl->id);
    } else {
        GERR("Failed to release %s request %u: %s", type->name, impl->id,
            GERRMSG(error));
        g_error_free(error);
    }

    nfc_request_impl_unref(impl);
}

static
void
nfc_request_impl_request_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcRequestImpl* impl = user_data;
    const NfcRequestType* type = impl->type;
    OrgSailfishosNfcDaemon* daemon = ORG_SAILFISHOS_NFC_DAEMON(proxy);
    GError* error = NULL;
    guint id = 0;

    GASSERT(impl->pending);
    impl->pending = FALSE;

    if (type->finish_request(daemon, &id, result, &error)) {
        if (impl->cancelled) {
            GDEBUG("%s request id %u (cancelled)", type->Name, id);
            impl->pending = TRUE;
            type->call_release(daemon, id, NULL,
                nfc_request_impl_release_done, nfc_request_impl_ref(impl));
        } else {
            GDEBUG("%s request id %u", type->Name, id);
            impl->id = id;
        }
    } else {
        GERR("Failed to request %s: %s", GERRMSG(error), type->name);
        g_error_free(error);
    }

    nfc_request_impl_unref(impl);
}

static
void
nfc_request_impl_present_changed(
    NfcDaemonClient* daemon,
    NFC_DAEMON_PROPERTY property,
    void* user_data)
{
    NfcRequestImpl* impl = user_data;

    if (daemon->present) {
        if (!impl->id && !impl->cancelled && !impl->pending) {
            NfcDaemonClientObject* self = impl->daemon;
            const NfcRequestType* type = impl->type;

            impl->pending = TRUE;
            type->call_request(self->proxy, impl->on, impl->off, NULL,
                nfc_request_impl_request_done, nfc_request_impl_ref(impl));
        }
    } else {
        impl->id = 0;
    }
}

static
NfcRequestImpl*
nfc_request_impl_new(
    NfcDaemonClientObject* self,
    const NfcRequestType* type,
    guint enable,
    guint disable)
{
    NfcRequestImpl* impl = g_slice_new0(NfcRequestImpl);
    NfcDaemonClient* client = &self->pub;

    g_atomic_int_set(&impl->refcount, 1);
    g_object_ref(impl->daemon = self);
    impl->on = enable;
    impl->off = disable;
    impl->type = type;
    impl->present_id = nfc_daemon_client_add_property_handler(client,
        NFC_DAEMON_PROPERTY_PRESENT, nfc_request_impl_present_changed,
        impl);
    if (client->present) {
        impl->pending = TRUE;
        type->call_request(self->proxy, impl->on, impl->off, NULL,
            nfc_request_impl_request_done, nfc_request_impl_ref(impl));
    }
    return impl;
}

static
void
nfc_request_impl_drop(
    NfcRequestImpl* impl)
{
    const NfcRequestType* type = impl->type;

    if (impl->pending) {
        impl->cancelled = TRUE;
        GDEBUG("Canceling pending %s request", type->name);
    } else if (impl->id) {
        NfcDaemonClientObject* self = impl->daemon;

        GDEBUG("Releasing %s request %u", type->name, impl->id);
        impl->pending = TRUE;
        type->call_release(self->proxy, impl->id, NULL,
            nfc_request_impl_release_done, nfc_request_impl_ref(impl));
    }

    nfc_request_impl_unref(impl);
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

GDBusConnection*
nfc_daemon_client_connection(
    NfcDaemonClient* daemon)
{
    return daemon ? nfc_daemon_client_object_cast(daemon)->connection : NULL;
}

#if NFCDC_NEED_PEER_SERVICE

void
nfc_daemon_client_register_service(
    NfcDaemonClient* daemon,
    NfcPeerService* service)
{
    NfcDaemonClientObject* self = nfc_daemon_client_object_cast(daemon);

    if (self->proxy && service) {
        org_sailfishos_nfc_daemon_call_register_local_service(self->proxy,
            service->path, service->sn, NULL,
            nfc_daemon_client_register_service_done,
            nfc_peer_service_ref(service));
    }
}

void
nfc_daemon_client_unregister_service(
    NfcDaemonClient* daemon,
    const char* path)
{
    NfcDaemonClientObject* self = nfc_daemon_client_object_cast(daemon);

    if (self->proxy && path) {
        org_sailfishos_nfc_daemon_call_unregister_local_service(self->proxy,
            path, NULL, NULL, NULL);
    }
}

#endif /* NFCDC_NEED_PEER_SERVICE */

/*==========================================================================*
 * API
 *==========================================================================*/

NfcDaemonClient*
nfc_daemon_client_new(
    void)
{
    if (nfc_daemon_client_instance) {
        g_object_ref(nfc_daemon_client_instance);
    } else {
        GError* error = NULL;
        NfcDaemonClientObject* self = g_object_new(THIS_TYPE, NULL);

        /* Acquire the bus synchronously */
        self->connection = g_bus_get_sync(NFCD_DBUS_TYPE, NULL, &error);
        if (self->connection) {
            GDEBUG("Bus connected");
            self->daemon_watch_initializing = TRUE;
            self->daemon_watch_id =
                g_bus_watch_name_on_connection(self->connection,
                    NFCD_DBUS_DAEMON_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                    nfc_daemon_client_daemon_appeared,
                    nfc_daemon_client_daemon_vanished,
                    self, NULL);
            self->settings_watch_initializing = TRUE;
            self->settings_watch_id =
                g_bus_watch_name_on_connection(self->connection,
                    NFCD_DBUS_SETTINGS_NAME, G_BUS_NAME_WATCHER_FLAGS_NONE,
                    nfc_daemon_client_settings_appeared,
                    nfc_daemon_client_settings_vanished,
                    self, NULL);
        } else {
            GERR("Failed to attach to NFC daemon bus: %s", GERRMSG(error));
            nfc_daemon_client_set_daemon_error(self, error);
        }
        nfc_daemon_client_update_valid_and_present(self);

        /* Clear pending signals since no one is listening yet */
        self->base.queued_signals = 0;

        nfc_daemon_client_instance = self;
    }
    return &nfc_daemon_client_instance->pub;
}

NfcDaemonClient*
nfc_daemon_client_ref(
    NfcDaemonClient* daemon)
{
    gutil_object_ref(nfc_daemon_client_object_cast(daemon));
    return daemon;
}

void
nfc_daemon_client_unref(
    NfcDaemonClient* daemon)
{
    gutil_object_unref(nfc_daemon_client_object_cast(daemon));
}

gboolean
nfc_daemon_client_register_local_host_service(
    NfcDaemonClient* daemon,
    const char* path,
    const char* name,
    GCancellable* cancel,
    NfcDaemonClientCallFunc callback,
    void* user_data,
    GDestroyNotify destroy) /* Since 1.1.0 */
{
    NfcDaemonClientObject* self = nfc_daemon_client_object_cast(daemon);

    if (G_LIKELY(self) && G_LIKELY(path) && g_variant_is_object_path(path) &&
        daemon->valid && daemon->present && (!cancel ||
        !g_cancellable_is_cancelled(cancel))) {
        NfcDaemonClientCallData* call = nfc_daemon_client_call_new(self,
            org_sailfishos_nfc_daemon_call_register_local_host_service_finish,
            cancel, callback, user_data, destroy);

        org_sailfishos_nfc_daemon_call_register_local_host_service
            (self->proxy, path, name ? name : "", cancel,
                nfc_daemon_client_call_done, call);
        return TRUE;
    } else {
        /* Destroy callback is always invoked even if we return FALSE */
        if (destroy) {
            destroy(user_data);
        }
        return FALSE;
    }
}

gboolean
nfc_daemon_client_unregister_local_host_service(
    NfcDaemonClient* daemon,
    const char* path,
    GCancellable* cancel,
    NfcDaemonClientCallFunc callback,
    void* user_data,
    GDestroyNotify destroy) /* Since 1.1.0 */
{
    NfcDaemonClientObject* self = nfc_daemon_client_object_cast(daemon);

    if (G_LIKELY(self) && G_LIKELY(path) && g_variant_is_object_path(path) &&
        daemon->valid && daemon->present && (!cancel ||
        !g_cancellable_is_cancelled(cancel))) {
        NfcDaemonClientCallData* call = nfc_daemon_client_call_new(self,
            org_sailfishos_nfc_daemon_call_unregister_local_host_service_finish,
            cancel, callback, user_data, destroy);

        org_sailfishos_nfc_daemon_call_unregister_local_host_service
            (self->proxy, path, cancel, nfc_daemon_client_call_done, call);
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
nfc_daemon_client_add_property_handler(
    NfcDaemonClient* daemon,
    NFC_DAEMON_PROPERTY property,
    NfcDaemonPropertyFunc callback,
    void* user_data)
{
    NfcDaemonClientObject* self = nfc_daemon_client_object_cast(daemon);

    return G_LIKELY(self) ? nfc_client_base_add_property_handler(&self->base,
        property, (NfcClientBasePropertyFunc) callback, user_data) : 0;
}

void
nfc_daemon_client_remove_handler(
    NfcDaemonClient* daemon,
    gulong id)
{
    if (G_LIKELY(id)) {
        NfcDaemonClientObject* self = nfc_daemon_client_object_cast(daemon);

        if (G_LIKELY(self)) {
            g_signal_handler_disconnect(self, id);
        }
    }
}

void
nfc_daemon_client_remove_handlers(
    NfcDaemonClient* daemon,
    gulong* ids,
    guint n)
{
    gutil_disconnect_handlers(nfc_daemon_client_object_cast(daemon), ids, n);
}

NfcModeRequest*
nfc_mode_request_new(
    NfcDaemonClient* daemon,
    NFC_MODE enable,
    NFC_MODE disable)  /* Since 1.0.6 */
{
    NfcDaemonClientObject* self = nfc_daemon_client_object_cast(daemon);

    if (G_LIKELY(self)) {
        static const NfcRequestType mode_request_type = {
            "Mode",
            "mode",
            org_sailfishos_nfc_daemon_call_request_mode,
            org_sailfishos_nfc_daemon_call_request_mode_finish,
            org_sailfishos_nfc_daemon_call_release_mode,
            org_sailfishos_nfc_daemon_call_release_mode_finish
        };

        NfcModeRequestPriv* priv = g_slice_new(NfcModeRequestPriv);
        NfcModeRequest* req = &priv->pub;

        req->enable = enable;
        req->disable = disable;
        priv->impl = nfc_request_impl_new(self, &mode_request_type,
            enable, disable);
        return req;
    }
    return NULL;
}

void
nfc_mode_request_free(
    NfcModeRequest* req)  /* Since 1.0.6 */
{
    if (req) {
        NfcModeRequestPriv* priv = G_CAST(req, NfcModeRequestPriv, pub);

        nfc_request_impl_drop(priv->impl);
        gutil_slice_free(priv);
    }
}

NfcTechRequest*
nfc_tech_request_new(
    NfcDaemonClient* daemon,
    NFC_TECH allow,
    NFC_TECH disallow)  /* Since 1.1.0 */
{
    NfcDaemonClientObject* self = nfc_daemon_client_object_cast(daemon);

    if (G_LIKELY(self)) {
        static const NfcRequestType tech_request_type = {
            "Tech",
            "tech",
            org_sailfishos_nfc_daemon_call_request_techs,
            org_sailfishos_nfc_daemon_call_request_techs_finish,
            org_sailfishos_nfc_daemon_call_release_techs,
            org_sailfishos_nfc_daemon_call_release_techs_finish
        };

        NfcTechRequestPriv* priv = g_slice_new(NfcTechRequestPriv);
        NfcTechRequest* req = &priv->pub;

        req->allow = allow;
        req->disallow = disallow;
        priv->impl = nfc_request_impl_new(self, &tech_request_type,
            allow, disallow);
        return req;
    }
    return NULL;
}

void
nfc_tech_request_free(
    NfcTechRequest* req)  /* Since 1.1.0 */
{
    if (req) {
        NfcTechRequestPriv* priv = G_CAST(req, NfcTechRequestPriv, pub);

        nfc_request_impl_drop(priv->impl);
        gutil_slice_free(priv);
    }
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
nfc_daemon_client_object_init(
    NfcDaemonClientObject* self)
{
    NfcDaemonClient* pub = &self->pub;

    GVERBOSE_("");
    pub->adapters = &nfc_daemon_client_empty_strv;
}

static
void
nfc_daemon_client_object_finalize(
    GObject* object)
{
    NfcDaemonClientObject* self = THIS(object);

    GVERBOSE_("");
    GASSERT(nfc_daemon_client_instance == self);
    nfc_daemon_client_instance = NULL;
    nfc_daemon_client_set_daemon_error(self, NULL);
    nfc_daemon_client_set_settings_error(self, NULL);
    nfc_daemon_client_drop_daemon_proxy(self);
    nfc_daemon_client_drop_settings_proxy(self);
    gutil_object_unref(self->connection);
    g_strfreev(self->adapters);
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
nfc_daemon_client_object_class_init(
    NfcDaemonClientObjectClass* klass)
{
    G_OBJECT_CLASS(klass)->finalize = nfc_daemon_client_object_finalize;
    klass->public_offset = G_STRUCT_OFFSET(NfcDaemonClientObject, pub);
    klass->valid_offset = G_STRUCT_OFFSET(NfcDaemonClientObject, pub.valid);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
