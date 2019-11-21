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

#include "nfcdc_daemon_p.h"
#include "nfcdc_dbus.h"
#include "nfcdc_base.h"
#include "nfcdc_log.h"

#include <gutil_macros.h>
#include <gutil_misc.h>
#include <gutil_strv.h>

#include "org.sailfishos.nfc.Daemon.h"
#include "org.sailfishos.nfc.Settings.h"

typedef NfcClientBaseClass NfcDaemonClientObjectClass;
typedef struct nfc_daemon_client_object {
    NfcClientBase base;
    NfcDaemonClient pub;
    GStrV* adapters;
    GError* daemon_error;
    GDBusConnection* connection;

    /* Daemon interface */
    OrgSailfishosNfcDaemon* daemon;
    gulong daemon_adapters_changed_id;
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

G_DEFINE_TYPE(NfcDaemonClientObject, nfc_daemon_client_object, \
        NFC_CLIENT_TYPE_BASE)
#define NFC_CLIENT_TYPE_DAEMON (nfc_daemon_client_object_get_type())
#define NFC_DAEMON_CLIENT_OBJECT(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj),\
	NFC_CLIENT_TYPE_DAEMON, NfcDaemonClientObject))

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
        NFC_DAEMON_CLIENT_OBJECT(G_CAST(pub, NfcDaemonClientObject, pub)) :
        NULL;
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
    } else if (self->daemon && self->settings) {
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
    GASSERT(!self->daemon || self->daemon == proxy);
    if (!gutil_strv_equal(self->adapters, adapters)) {
        NfcDaemonClient* pub = &self->pub;

        DUMP_STRV(NULL, "Adapters", "=", adapters);
        g_strfreev(self->adapters);
        pub->adapters = self->adapters = g_strdupv(adapters);
        nfc_daemon_client_queue_signal_(self, ADAPTERS);
        nfc_daemon_client_emit_queued_signals(self);
    }
}

static
void
nfc_daemon_client_new_daemon_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcDaemonClientObject* self = NFC_DAEMON_CLIENT_OBJECT(user_data);
    OrgSailfishosNfcDaemon* daemon = ORG_SAILFISHOS_NFC_DAEMON(proxy);
    GError* error = NULL;
    char** adapters = NULL;

    GASSERT(!self->daemon);
    GASSERT(self->daemon_adapters_changed_id);
    if (org_sailfishos_nfc_daemon_call_get_adapters_finish(daemon,
        &adapters, result, &error)) {
        NfcDaemonClient* pub = &self->pub;

        if (!gutil_strv_equal(self->adapters, adapters)) {
            g_strfreev(self->adapters);
            pub->adapters = self->adapters = adapters;
            nfc_daemon_client_queue_signal_(self, ADAPTERS);
        } else {
            g_strfreev(adapters);
        }
        self->daemon = daemon;
    } else {
        GERR("Failed to query NFC adapters: %s", GERRMSG(error));
        nfc_daemon_client_set_daemon_error(self, error);
        gutil_disconnect_handlers(self->daemon,
            &self->daemon_adapters_changed_id, 1);
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
    NfcDaemonClientObject* self = NFC_DAEMON_CLIENT_OBJECT(user_data);
    GError* error = NULL;
    OrgSailfishosNfcDaemon* daemon =
        org_sailfishos_nfc_daemon_proxy_new_finish(result, &error);

    if (daemon) {
        GDEBUG("Connected to NFC daemon");
        self->daemon_adapters_changed_id =
            g_signal_connect(daemon, "adapters-changed",
                G_CALLBACK(nfc_daemon_client_daemon_adapters_changed), self);
        org_sailfishos_nfc_daemon_call_get_adapters(daemon, NULL,
            nfc_daemon_client_new_daemon_done, g_object_ref(self));
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
    NfcDaemonClientObject* self = NFC_DAEMON_CLIENT_OBJECT(user_data);
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
    NfcDaemonClientObject* self = NFC_DAEMON_CLIENT_OBJECT(user_data);
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

    if (self->daemon) {
        gutil_disconnect_handlers(self->daemon,
            &self->daemon_adapters_changed_id, 1);
        g_object_unref(self->daemon);
        self->daemon = NULL;
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
    NfcDaemonClientObject* self = NFC_DAEMON_CLIENT_OBJECT(user_data);

    GDEBUG("Name '%s' is owned by %s", name, owner);
    GASSERT(!self->daemon);
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
    NfcDaemonClientObject* self = NFC_DAEMON_CLIENT_OBJECT(user_data);

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
    NfcDaemonClientObject* self = NFC_DAEMON_CLIENT_OBJECT(user_data);

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
    NfcDaemonClientObject* self = NFC_DAEMON_CLIENT_OBJECT(user_data);

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

static
void
nfc_daemon_client_new_bus(
    GObject* object,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcDaemonClientObject* self = NFC_DAEMON_CLIENT_OBJECT(user_data);
    GError* error = NULL;

    self->connection = g_bus_get_finish(result, &error);
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
    nfc_daemon_client_emit_queued_signals(self);
    g_object_unref(self);
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
        /* Start the initialization sequence */
        nfc_daemon_client_instance = g_object_new(NFC_CLIENT_TYPE_DAEMON, NULL);
        g_bus_get(NFCD_DBUS_TYPE, NULL, nfc_daemon_client_new_bus,
            g_object_ref(nfc_daemon_client_instance));
    }
    return &nfc_daemon_client_instance->pub;
}

NfcDaemonClient*
nfc_daemon_client_ref(
    NfcDaemonClient* daemon)
{
    if (G_LIKELY(daemon)) {
        g_object_ref(nfc_daemon_client_object_cast(daemon));
        return daemon;
    } else {
        return NULL;
    }
}

void
nfc_daemon_client_unref(
    NfcDaemonClient* daemon)
{
    if (G_LIKELY(daemon)) {
        g_object_unref(nfc_daemon_client_object_cast(daemon));
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
    NfcDaemonClientObject* self = NFC_DAEMON_CLIENT_OBJECT(object);

    GVERBOSE_("");
    GASSERT(nfc_daemon_client_instance == self);
    nfc_daemon_client_instance = NULL;
    nfc_daemon_client_set_daemon_error(self, NULL);
    nfc_daemon_client_set_settings_error(self, NULL);
    nfc_daemon_client_drop_daemon_proxy(self);
    nfc_daemon_client_drop_settings_proxy(self);
    if (self->connection) {
        g_object_unref(self->connection);
    }
    g_strfreev(self->adapters);
    G_OBJECT_CLASS(nfc_daemon_client_object_parent_class)->finalize(object);
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
