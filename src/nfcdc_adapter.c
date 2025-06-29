/*
 * Copyright (C) 2019-2025 Slava Monich <slava@monich.com>
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

#include "nfcdc_adapter_p.h"
#include "nfcdc_base.h"
#include "nfcdc_daemon_p.h"
#include "nfcdc_dbus.h"
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
    PROXY_PEERS_CHANGED,
    PROXY_HOSTS_CHANGED,
    PROXY_PARAM_CHANGED,
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
    GStrV* peers;
    GStrV* hosts;
    GUtilData* la_nfcid1;
    GDBusConnection* connection;
    OrgSailfishosNfcAdapter* proxy;
    gulong proxy_signal_id[PROXY_SIGNAL_COUNT];
    gboolean proxy_initializing;
} NfcAdapterClientObject;

#define PARENT_CLASS nfc_adapter_client_object_parent_class
#define THIS_TYPE nfc_adapter_client_object_get_type()
#define THIS(obj) G_TYPE_CHECK_INSTANCE_CAST(obj, THIS_TYPE, \
    NfcAdapterClientObject)

GType THIS_TYPE G_GNUC_INTERNAL;
G_DEFINE_TYPE(NfcAdapterClientObject, nfc_adapter_client_object, \
    NFC_CLIENT_TYPE_BASE)

NFC_CLIENT_BASE_ASSERT_VALID(NFC_ADAPTER_PROPERTY_VALID);
NFC_CLIENT_BASE_ASSERT_COUNT(NFC_ADAPTER_PROPERTY_COUNT);

#define SIGNAL_BIT_(x) NFC_CLIENT_BASE_SIGNAL_BIT(NFC_ADAPTER_PROPERTY_##x)

#define nfc_adapter_client_emit_queued_signals(self) \
    nfc_client_base_emit_queued_signals(&(self)->base)
#define nfc_adapter_client_queue_signal(self,NAME) \
    ((self)->base.queued_signals |= SIGNAL_BIT_(NAME))
#define nfc_adapter_client_signal_property_change(self,NAME) \
    nfc_client_base_signal_property_change(&(self)->base, \
    NFC_ADAPTER_PROPERTY_##NAME)

static char* nfc_adapter_client_empty_strv = NULL;
static GHashTable* nfc_adapter_client_table;

static const char PARAM_T4_NDEF[] = "T4_NDEF";
static const char PARAM_LA_NFCID1[] = "LA_NFCID1";

struct nfc_adapter_param_req {
    gint ref_count;
    NfcAdapterClientObject* client;
    GVariant* params;
    gboolean pending;
    gboolean reset;
    gulong valid_id;
    guint id;
};

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
    NfcAdapterClient* adapter)
{
    return G_LIKELY(adapter) ?
        THIS(G_CAST(adapter, NfcAdapterClientObject, pub)) :
        NULL;
}

static
void
nfc_adapter_client_update_tags(
    NfcAdapterClientObject* self,
    const GStrV* tags,
    char** take_tags)
{
    NfcAdapterClient* adapter = &self->pub;

    GASSERT(!take_tags || take_tags == tags);
    if (!gutil_strv_equal(adapter->tags, tags)) {
        DUMP_STRV(self->name, "Tags", "=", tags);
        g_strfreev(self->tags);
        if (tags && tags[0]) {
            if (take_tags) {
                self->tags = take_tags;
                take_tags = NULL;
            } else {
                self->tags = g_strdupv((char**)tags);
            }
            adapter->tags = self->tags;
        } else {
            adapter->tags = &nfc_adapter_client_empty_strv;
            self->tags = NULL;
        }
        nfc_adapter_client_queue_signal(self, TAGS);
    }
    g_strfreev(take_tags);
}

static
void
nfc_adapter_client_update_peers(
    NfcAdapterClientObject* self,
    const GStrV* peers,
    char** take_peers)
{
    NfcAdapterClient* adapter = &self->pub;

    GASSERT(!take_peers || take_peers == peers);
    if (!gutil_strv_equal(adapter->peers, peers)) {
        DUMP_STRV(self->name, "Peers", "=", peers);
        g_strfreev(self->peers);
        if (peers && peers[0]) {
            if (take_peers) {
                self->peers = take_peers;
                take_peers = NULL;
            } else {
                self->peers = g_strdupv((char**)peers);
            }
            adapter->peers = self->peers;
        } else {
            adapter->peers = &nfc_adapter_client_empty_strv;
            self->peers = NULL;
        }
        nfc_adapter_client_queue_signal(self, PEERS);
    }
    g_strfreev(take_peers);
}

static
void
nfc_adapter_client_update_hosts(
    NfcAdapterClientObject* self,
    const GStrV* hosts,
    char** take_hosts)
{
    NfcAdapterClient* adapter = &self->pub;

    GASSERT(!take_hosts || take_hosts == hosts);
    if (!gutil_strv_equal(adapter->hosts, hosts)) {
        DUMP_STRV(self->name, "Hosts", "=", hosts);
        g_strfreev(self->hosts);
        if (hosts && hosts[0]) {
            if (take_hosts) {
                self->hosts = take_hosts;
                take_hosts = NULL;
            } else {
                self->hosts = g_strdupv((char**)hosts);
            }
            adapter->hosts = self->hosts;
        } else {
            adapter->hosts = &nfc_adapter_client_empty_strv;
            self->hosts = NULL;
        }
        nfc_adapter_client_queue_signal(self, HOSTS);
    }
    g_strfreev(take_hosts);
}

static
void
nfc_adapter_client_update_valid_and_present(
    NfcAdapterClientObject* self)
{
    NfcAdapterClient* adapter = &self->pub;
    NfcDaemonClient* daemon = self->daemon;
    gboolean valid, present;

    if (!daemon->valid || self->proxy_initializing) {
        valid = FALSE;
        present = FALSE;
    } else {
        valid = TRUE;
        present = (self->proxy && daemon->present &&
            gutil_strv_contains(daemon->adapters, adapter->path));
    }
    if (adapter->valid != valid) {
        adapter->valid = valid;
        nfc_adapter_client_queue_signal(self, VALID);
    }
    if (adapter->present != present) {
        adapter->present = present;
        nfc_adapter_client_queue_signal(self, PRESENT);
    }
}

static
void
nfc_adapter_client_update_t4_ndef(
    NfcAdapterClientObject* self,
    GVariant* value)
{
    GVariant* tmp = NULL;

    if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
        value = tmp = g_variant_get_variant(value);
    }

    if  (g_variant_is_of_type(value, G_VARIANT_TYPE_BOOLEAN)) {
        NfcAdapterClient* adapter = &self->pub;
        const gboolean b = g_variant_get_boolean(value);

        if (adapter->t4_ndef != b) {
            adapter->t4_ndef = b;
            GDEBUG("%s: T4_NDEF = %s", self->name, b ? "on" : "off");
            nfc_adapter_client_queue_signal(self, T4_NDEF);
        }
    }

    if (tmp) {
        g_variant_unref(tmp);
    }
}

static
void
nfc_adapter_client_update_la_nfcid1(
    NfcAdapterClientObject* self,
    GVariant* value)
{
    GVariant* tmp = NULL;

    if (g_variant_is_of_type(value, G_VARIANT_TYPE_VARIANT)) {
        value = tmp = g_variant_get_variant(value);
    }

    if (g_variant_is_of_type(value, G_VARIANT_TYPE_BYTESTRING)) {
        NfcAdapterClient* adapter = &self->pub;
        GUtilData data;

        data.bytes = g_variant_get_data(value);
        data.size = g_variant_get_size(value);
        if (!gutil_data_equal(self->la_nfcid1, &data)) {
            g_free(self->la_nfcid1);
            adapter->la_nfcid1 = self->la_nfcid1 = gutil_data_copy(&data);
            DUMP_DATA(self->name, "LA_NFCID1", "=", self->la_nfcid1);
            nfc_adapter_client_queue_signal(self, LA_NFCID1);
        }
    }

    if (tmp) {
        g_variant_unref(tmp);
    }
}

static
void
nfc_adapter_client_init_finished(
    NfcAdapterClientObject* self,
    gboolean enabled,
    gboolean powered,
    NFC_MODE supported_modes,
    NFC_MODE mode,
    gboolean target_present,
    char** take_tags,
    char** take_peers,
    char** take_hosts,
    NFC_TECH supported_techs,
    GVariant* params)
{
    NfcAdapterClient* adapter = &self->pub;

    if (adapter->enabled != enabled) {
        adapter->enabled = enabled;
        nfc_adapter_client_queue_signal(self, ENABLED);
    }
    if (adapter->powered != powered) {
        adapter->powered = powered;
        nfc_adapter_client_queue_signal(self, POWERED);
    }
    if (adapter->mode != mode) {
        adapter->mode = mode;
        nfc_adapter_client_queue_signal(self, MODE);
    }
    if (adapter->target_present != target_present) {
        adapter->target_present = target_present;
        nfc_adapter_client_queue_signal(self, TARGET_PRESENT);
    }
    adapter->supported_modes = supported_modes;
    adapter->supported_techs = supported_techs;
    nfc_adapter_client_update_tags(self, take_tags, take_tags);
    nfc_adapter_client_update_peers(self, take_peers, take_peers);
    nfc_adapter_client_update_hosts(self, take_hosts, take_hosts);
    if (params) {
        GVariantIter it;
        GVariant* entry;

        for (g_variant_iter_init(&it, params);
             (entry = g_variant_iter_next_value(&it)) != NULL;
             g_variant_unref(entry)) {
            GVariant* dict_key = g_variant_get_child_value(entry, 0);
            GVariant* dict_value = g_variant_get_child_value(entry, 1);
            const char* name = g_variant_get_string(dict_key, NULL);

            if (!g_strcmp0(name, PARAM_T4_NDEF)) {
                nfc_adapter_client_update_t4_ndef(self, dict_value);
            } else if (!g_strcmp0(name, PARAM_LA_NFCID1)) {
                nfc_adapter_client_update_la_nfcid1(self, dict_value);
            } else {
                GDEBUG("%s: Unexpected adapter param '%s'", self->name, name);
            }
            g_variant_unref(dict_key);
            g_variant_unref(dict_value);
        }
    }
}

static
void
nfc_adapter_client_drop_proxy(
    NfcAdapterClientObject* self)
{
    NfcAdapterClient* adapter = &self->pub;

    GASSERT(!self->proxy_initializing);
    if (self->proxy) {
        gutil_disconnect_handlers(self->proxy, self->proxy_signal_id,
            G_N_ELEMENTS(self->proxy_signal_id));
        g_object_unref(self->proxy);
        self->proxy = NULL;
    }
    if (adapter->valid) {
        adapter->valid = FALSE;
        nfc_adapter_client_queue_signal(self, VALID);
    }
    if (adapter->present) {
        adapter->present = FALSE;
        nfc_adapter_client_queue_signal(self, PRESENT);
    }
    nfc_adapter_client_init_finished(self, FALSE, FALSE, NFC_MODE_NONE,
        NFC_MODE_NONE, FALSE, NULL, NULL, NULL, NFC_TECH_NONE, NULL);
}

static
void
nfc_adapter_client_update(
    NfcAdapterClientObject* self)
{
    NfcAdapterClient* adapter = &self->pub;

    if (gutil_strv_contains(self->daemon->adapters, adapter->path)) {
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
    NfcAdapterClientObject* self = THIS(user_data);

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
    NfcAdapterClient* adapter = &self->pub;

    /* We may receive a signal before the initial query has completed */
    GASSERT(!self->proxy || self->proxy == proxy);
    if (adapter->enabled != enabled) {
        adapter->enabled = enabled;
        GDEBUG("%s: %sabled", self->name, enabled ? "En" : "Dis");
        nfc_adapter_client_signal_property_change(self, ENABLED);
    }
}

static
void
nfc_adapter_client_powered_changed(
    OrgSailfishosNfcAdapter* proxy,
    gboolean powered,
    NfcAdapterClientObject* self)
{
    NfcAdapterClient* adapter = &self->pub;

    /* We may receive a signal before the initial query has completed */
    GASSERT(!self->proxy || self->proxy == proxy);
    if (adapter->powered != powered) {
        adapter->powered = powered;
        GDEBUG("%s: Powered = %s", self->name, powered ? "on" : "off");
        nfc_adapter_client_signal_property_change(self, POWERED);
    }
}

static
void
nfc_adapter_client_mode_changed(
    OrgSailfishosNfcAdapter* proxy,
    NFC_MODE mode,
    NfcAdapterClientObject* self)
{
    NfcAdapterClient* adapter = &self->pub;

    /* We may receive a signal before the initial query has completed */
    GASSERT(!self->proxy || self->proxy == proxy);
    if (adapter->mode != mode) {
        adapter->mode = mode;
        GDEBUG("%s: Mode = 0x%02x", self->name, mode);
        nfc_adapter_client_signal_property_change(self, MODE);
    }
}

static
void
nfc_adapter_client_target_present_changed(
    OrgSailfishosNfcAdapter* proxy,
    gboolean present,
    NfcAdapterClientObject* self)
{
    NfcAdapterClient* adapter = &self->pub;

    /* We may receive a signal before the initial query has completed */
    GASSERT(!self->proxy || self->proxy == proxy);
    if (adapter->target_present != present) {
        adapter->target_present = present;
        GDEBUG("%s: Target = %sresent", self->name, present ? "P" : "Not p");
        nfc_adapter_client_signal_property_change(self, TARGET_PRESENT);
    }
}

static
void
nfc_adapter_client_tags_changed(
    OrgSailfishosNfcAdapter* proxy,
    const GStrV* tags,
    NfcAdapterClientObject* self)
{
    /* We may receive a signal before the initial query has completed */
    GASSERT(!self->proxy || self->proxy == proxy);
    nfc_adapter_client_update_tags(self, tags, NULL);
    nfc_adapter_client_emit_queued_signals(self);
}

static
void
nfc_adapter_client_peers_changed(
    OrgSailfishosNfcAdapter* proxy,
    const GStrV* peers,
    NfcAdapterClientObject* self)
{
    /* We may receive a signal before the initial query has completed */
    GASSERT(!self->proxy || self->proxy == proxy);
    nfc_adapter_client_update_peers(self, peers, NULL);
    nfc_adapter_client_emit_queued_signals(self);
}

static
void
nfc_adapter_client_hosts_changed(
    OrgSailfishosNfcAdapter* proxy,
    const GStrV* hosts,
    NfcAdapterClientObject* self)
{
    /* We may receive a signal before the initial query has completed */
    GASSERT(!self->proxy || self->proxy == proxy);
    nfc_adapter_client_update_hosts(self, hosts, NULL);
    nfc_adapter_client_emit_queued_signals(self);
}

static
void
nfc_adapter_client_param_changed(
    OrgSailfishosNfcAdapter* proxy,
    const char* name,
    GVariant* value,
    NfcAdapterClientObject* self)
{
    /* We may receive a signal before the initial query has completed */
    GASSERT(!self->proxy || self->proxy == proxy);

    if (!g_strcmp0(name, PARAM_T4_NDEF)) {
        nfc_adapter_client_update_t4_ndef(self, value);
    } else if (!g_strcmp0(name, PARAM_LA_NFCID1)) {
        nfc_adapter_client_update_la_nfcid1(self, value);
    }
    nfc_adapter_client_emit_queued_signals(self);
}

static
void
nfc_adapter_client_get_all4_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcAdapterClientObject* self = THIS(user_data);
    GError* error = NULL;
    gint version;
    guint supported_modes, mode, supported_techs;
    gboolean enabled, powered, target_present;
    gchar** tags;
    gchar** peers;
    gchar** hosts;
    GVariant* params;

    GASSERT(self->proxy_initializing);
    self->proxy_initializing = FALSE;
    if (org_sailfishos_nfc_adapter_call_get_all4_finish(self->proxy, &version,
        &enabled, &powered, &supported_modes, &mode, &target_present, &tags,
        &peers, &hosts, &supported_techs, &params, result, &error)) {
        GASSERT(self->pub.version == version);
        GDEBUG("%s: Modes = 0x%02x", self->name, supported_modes);
        GDEBUG("%s: Techs = 0x%02x", self->name, supported_techs);
        /* Passing ownership of tags, peers and hosts to self */
        nfc_adapter_client_init_finished(self, enabled, powered,
            supported_modes, mode, target_present, tags, peers, hosts,
            supported_techs, params);
        g_variant_unref(params);
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
nfc_adapter_client_get_all3_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcAdapterClientObject* self = THIS(user_data);
    GError* error = NULL;
    gint version;
    guint supported_modes, mode, supported_techs;
    gboolean enabled, powered, target_present;
    gchar** tags;
    gchar** peers;
    gchar** hosts;

    GASSERT(self->proxy_initializing);
    self->proxy_initializing = FALSE;
    if (org_sailfishos_nfc_adapter_call_get_all3_finish(self->proxy, &version,
        &enabled, &powered, &supported_modes, &mode, &target_present, &tags,
        &peers, &hosts, &supported_techs, result, &error)) {
        GASSERT(self->pub.version == version);
        GDEBUG("%s: Modes = 0x%02x", self->name, supported_modes);
        GDEBUG("%s: Techs = 0x%02x", self->name, supported_techs);
        /* Passing ownership of tags, peers and hosts to self */
        nfc_adapter_client_init_finished(self, enabled, powered,
            supported_modes, mode, target_present, tags, peers, hosts,
            supported_techs, NULL);
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
nfc_adapter_client_get_all2_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcAdapterClientObject* self = THIS(user_data);
    GError* error = NULL;
    gint version;
    guint supported_modes, mode;
    gboolean enabled, powered, target_present;
    gchar** tags;
    gchar** peers;

    GASSERT(self->proxy_initializing);
    self->proxy_initializing = FALSE;
    if (org_sailfishos_nfc_adapter_call_get_all2_finish(self->proxy, &version,
        &enabled, &powered, &supported_modes, &mode, &target_present, &tags,
        &peers, result, &error)) {
        GASSERT(self->pub.version == version);
        GDEBUG("%s: Modes = 0x%02x", self->name, supported_modes);
        /* Passing ownership of tags and peers to self */
        nfc_adapter_client_init_finished(self, enabled, powered,
            supported_modes, mode, target_present, tags, peers, NULL,
            NFC_TECH_NONE, NULL);
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
nfc_adapter_client_get_all_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcAdapterClientObject* self = THIS(user_data);
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
        GASSERT(self->pub.version == version);
        /* Passing ownership of tags to self */
        nfc_adapter_client_init_finished(self, enabled, powered,
            supported_modes, mode, target_present, tags, NULL, NULL,
            NFC_TECH_NONE, NULL);
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
nfc_adapter_client_get_interface_version_done(
    GObject* proxy,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcAdapterClientObject* self = THIS(user_data);
    NfcAdapterClient* adapter = &self->pub;
    GError* error = NULL;

    if (org_sailfishos_nfc_adapter_call_get_interface_version_finish
       (self->proxy, &adapter->version, result, &error)) {
        GDEBUG("org.sailfishos.nfc.Adapter v%d", adapter->version);
        GASSERT(self->proxy_initializing);
        if (adapter->version >= 4) {
            org_sailfishos_nfc_adapter_call_get_all4(self->proxy, NULL,
                nfc_adapter_client_get_all4_done, g_object_ref(self));
        } else if (adapter->version >= 3) {
            org_sailfishos_nfc_adapter_call_get_all3(self->proxy, NULL,
                nfc_adapter_client_get_all3_done, g_object_ref(self));
        } else if (adapter->version >= 2) {
            org_sailfishos_nfc_adapter_call_get_all2(self->proxy, NULL,
                nfc_adapter_client_get_all2_done, g_object_ref(self));
        } else {
            org_sailfishos_nfc_adapter_call_get_all(self->proxy, NULL,
                nfc_adapter_client_get_all_done, g_object_ref(self));
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
nfc_adapter_client_init_2(
    GObject* connection,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcAdapterClientObject* self = THIS(user_data);
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
        self->proxy_signal_id[PROXY_PEERS_CHANGED] =
            g_signal_connect(self->proxy, "peers-changed",
                G_CALLBACK(nfc_adapter_client_peers_changed), self);
        self->proxy_signal_id[PROXY_HOSTS_CHANGED] =
            g_signal_connect(self->proxy, "hosts-changed",
                G_CALLBACK(nfc_adapter_client_hosts_changed), self);
        self->proxy_signal_id[PROXY_PARAM_CHANGED] =
            g_signal_connect(self->proxy, "param-changed",
                G_CALLBACK(nfc_adapter_client_param_changed), self);
        org_sailfishos_nfc_adapter_call_get_interface_version(self->proxy,
            NULL, nfc_adapter_client_get_interface_version_done,
            g_object_ref(self));
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
nfc_adapter_client_init_1(
    NfcAdapterClientObject* self)
{
    org_sailfishos_nfc_adapter_proxy_new(self->connection,
        G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, NFCD_DBUS_DAEMON_NAME,
        self->pub.path, NULL, nfc_adapter_client_init_2, g_object_ref(self));
}


static
void
nfc_adapter_client_reinit(
    NfcAdapterClientObject* self)
{
    GASSERT(!self->proxy_initializing);
    self->proxy_initializing = TRUE;
    nfc_adapter_client_init_1(self);
}

/*==========================================================================*
 * Param request
 *==========================================================================*/

static
NfcAdapterParamReq*
nfc_adapter_param_req_ref(
    NfcAdapterParamReq* req)
{
    if (G_LIKELY(req)) {
        g_atomic_int_inc(&req->ref_count);
    }
    return req;
}

static
void
nfc_adapter_param_req_unref(
    NfcAdapterParamReq* req)
{
    if (G_LIKELY(req) && g_atomic_int_dec_and_test(&req->ref_count)) {
        if (req->id && req->client->proxy) {
            GDEBUG("%s: Releasing param req %u", req->client->name, req->id);
            org_sailfishos_nfc_adapter_call_release_params(req->client->proxy,
                req->id, NULL, NULL, NULL);
        }
        g_variant_unref(req->params);
        g_object_unref(req->client);
        gutil_slice_free(req);
    }
}

static
void
nfc_adapter_param_req_complete(
    GObject* object,
    GAsyncResult* result,
    gpointer user_data)
{
    NfcAdapterParamReq* req = user_data;
    OrgSailfishosNfcAdapter* proxy = ORG_SAILFISHOS_NFC_ADAPTER(object);
    GError* error = NULL;
    guint id = 0;

    req->pending = FALSE;
    if (org_sailfishos_nfc_adapter_call_request_params_finish(proxy, &id,
        result, &error)) {
        if (req->id) {
            GDEBUG("%s: Dropping param req %u", req->client->name, req->id);
            org_sailfishos_nfc_adapter_call_release_params(req->client->proxy,
                req->id, NULL, NULL, NULL);
        }
        GDEBUG("%s: Param req id %u", req->client->name, id);
        req->id = id;
    } else {
        GERR("%s", GERRMSG(error));
        g_error_free(error);
    }
    nfc_adapter_param_req_unref(req);
}

static
void
nfc_adapter_param_req_submit(
    NfcAdapterParamReq* req)
{
    if (!req->pending) {
        req->pending = TRUE;
        org_sailfishos_nfc_adapter_call_request_params(req->client->proxy,
            req->params, req->reset, NULL, nfc_adapter_param_req_complete,
            nfc_adapter_param_req_ref(req));
    }
}

static
void
nfc_adapter_param_req_update_valid(
    NfcAdapterClient* adapter,
    NFC_ADAPTER_PROPERTY property,
    void* user_data)
{
    NfcAdapterParamReq* req = user_data;

    if (adapter->valid) {
        nfc_adapter_param_req_submit(req);
    } else {
        req->pending = FALSE; /* Allow to submit next request right away */
        req->id = 0;
    }
}

static
NfcAdapterParamReq*
nfc_adapter_param_req_alloc(
    NfcAdapterClient* adapter,
    gboolean reset,
    GVariant* params)
{
    NfcAdapterClientObject* client = nfc_adapter_client_object_cast(adapter);
    NfcAdapterParamReq* req = g_slice_new0(NfcAdapterParamReq);

    g_atomic_int_set(&req->ref_count, 1);
    g_object_ref(req->client = client);
    req->reset = reset;
    req->params = g_variant_ref_sink(params);
    req->valid_id = nfc_adapter_client_add_property_handler(adapter,
        NFC_ADAPTER_PROPERTY_VALID, nfc_adapter_param_req_update_valid, req);
    if (adapter->valid) {
        nfc_adapter_param_req_submit(req);
    }
    return req;
}

static
void
nfc_adapter_param_add(
    GVariantBuilder* builder,
    NFC_ADAPTER_PARAM_KEY key,
    const NfcAdapterParamValue* value)
{
    static const char* format = "{sv}";

    /* Append known params to GVariant dictionary */
    switch (key) {
    case NFC_ADAPTER_PARAM_KEY_NONE:
        break;
    case NFC_ADAPTER_PARAM_KEY_T4_NDEF:
        g_variant_builder_add(builder, format, PARAM_T4_NDEF,
            g_variant_new_boolean(value->b));
        break;
    case NFC_ADAPTER_PARAM_KEY_LA_NFCID1:
        if (value->data.size) {
            g_variant_builder_add(builder, format, PARAM_LA_NFCID1,
                g_variant_new_fixed_array(G_VARIANT_TYPE_BYTE,
                    value->data.bytes, value->data.size, 1));
        } else {
            g_variant_builder_add(builder, format, PARAM_LA_NFCID1,
                g_variant_new_from_data(G_VARIANT_TYPE_BYTESTRING,
                    NULL, 0, TRUE, NULL, NULL));
        }
        break;
    }
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
        NfcAdapterClientObject* self = NULL;

        if (nfc_adapter_client_table) {
            self = g_hash_table_lookup(nfc_adapter_client_table, path);
        }
        if (self) {
            g_object_ref(self);
            return &self->pub;
        } else {
            self = g_object_new(THIS_TYPE, NULL);
            if (self->connection) {
                char* key = g_strdup(path);

                GVERBOSE_("%s", path);
                if (!nfc_adapter_client_table) {
                    nfc_adapter_client_table = g_hash_table_new_full(g_str_hash,
                        g_str_equal, g_free, NULL);
                }
                g_hash_table_insert(nfc_adapter_client_table, key, self);
                self->pub.path = key;
                self->name = key + 1;
                nfc_adapter_client_update(self);
                nfc_adapter_client_init_1(self);

                /* Clear pending signals since no one is listening yet */
                self->base.queued_signals = 0;

                return &self->pub;
            }
            g_object_unref(self);
        }
    }
    return NULL;
}


NfcAdapterClient*
nfc_adapter_client_ref(
    NfcAdapterClient* adapter)
{
    gutil_object_ref(nfc_adapter_client_object_cast(adapter));
    return adapter;
}

void
nfc_adapter_client_unref(
    NfcAdapterClient* adapter)
{
    gutil_object_unref(nfc_adapter_client_object_cast(adapter));
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

NfcAdapterParamReq*
nfc_adapter_param_req_new(
    NfcAdapterClient* adapter,
    gboolean reset,
    const NfcAdapterParamPtrC* params) /* Since 1.2.0 */
{
    if (adapter && (reset || (params && params[0]))) {
        GVariantBuilder dict;

        g_variant_builder_init(&dict, G_VARIANT_TYPE_VARDICT);
        if (params) {
            const NfcAdapterParamPtrC* ptr = params;

            /* The list is NULL terminated */
            while (*ptr) {
                const NfcAdapterParam* p = *ptr++;

                nfc_adapter_param_add(&dict, p->key, &p->value);
            }
        }
        return nfc_adapter_param_req_alloc(adapter, reset,
            g_variant_builder_end(&dict));
    }
    return NULL;
}

void
nfc_adapter_param_req_free(
    NfcAdapterParamReq* req) /* Since 1.2.0 */
{
    nfc_adapter_param_req_unref(req);
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
nfc_adapter_client_object_init(
    NfcAdapterClientObject* self)
{
    NfcAdapterClient* adapter = &self->pub;

    adapter->tags = &nfc_adapter_client_empty_strv;
    adapter->peers = &nfc_adapter_client_empty_strv;
    adapter->hosts = &nfc_adapter_client_empty_strv;
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
    gutil_object_ref(self->connection =
        nfc_daemon_client_connection(self->daemon));
    adapter->t4_ndef = TRUE;
}

static
void
nfc_adapter_client_object_finalize(
    GObject* object)
{
    NfcAdapterClientObject* self = THIS(object);
    NfcAdapterClient* adapter = &self->pub;

    GVERBOSE_("%s", adapter->path);
    nfc_adapter_client_drop_proxy(self);
    nfc_daemon_client_remove_all_handlers(self->daemon, self->daemon_event_id);
    nfc_daemon_client_unref(self->daemon);
    gutil_object_unref(self->connection);
    g_strfreev(self->tags);
    g_strfreev(self->peers);
    g_strfreev(self->hosts);
    g_free(self->la_nfcid1);
    if (adapter->path) {
        g_hash_table_remove(nfc_adapter_client_table, adapter->path);
        if (g_hash_table_size(nfc_adapter_client_table) == 0) {
            g_hash_table_unref(nfc_adapter_client_table);
            nfc_adapter_client_table = NULL;
        }
    }
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
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
