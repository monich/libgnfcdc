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

/* This API exists since 1.0.6 */

#include "nfcdc_base.h"
#include "nfcdc_daemon_p.h"
#include "nfcdc_log.h"
#include "nfcdc_peer_service_p.h"

#include "org.sailfishos.nfc.LocalService.h"

#include <gutil_macros.h>
#include <gutil_misc.h>

#include <gio/gunixfdlist.h>

#include <sys/socket.h>

enum nfc_peer_service_daemon_events {
    DAEMON_PRESENT_CHANGED,
    DAEMON_SIGNAL_COUNT
};

enum nfc_peer_service_calls {
    CALL_ACCEPT,
    CALL_PEER_ARRIVED,
    CALL_PEER_LEFT,
    CALL_COUNT
};

typedef NfcClientBaseClass NfcPeerServiceObjectClass;
typedef struct nfc_peer_service_object {
    NfcClientBase base;
    NfcPeerService pub;
    NfcDaemonClient* daemon;
    NfcPeerServiceHandlerFunc handler;
    void* handler_data;
    OrgSailfishosNfcLocalService* object;
    gulong call_id[CALL_COUNT];
    gulong daemon_event_id[CALL_COUNT];
    gboolean exported;
    char* path;
    char* sn;
} NfcPeerServiceObject;

typedef struct nfc_service_connection_priv {
    NfcServiceConnection pub;
    OrgSailfishosNfcLocalService* object;
    GDBusMethodInvocation* accept_call;
    gint refcount;
} NfcServiceConnectionPriv;

G_DEFINE_TYPE(NfcPeerServiceObject, nfc_peer_service_object, \
    NFC_CLIENT_TYPE_BASE)
#define PARENT_CLASS nfc_peer_service_object_parent_class
#define THIS_TYPE (nfc_peer_service_object_get_type())
#define THIS(obj) (G_TYPE_CHECK_INSTANCE_CAST((obj), THIS_TYPE, \
    NfcPeerServiceObject))

NFC_CLIENT_BASE_ASSERT_COUNT(NFC_PEER_SERVICE_PROPERTY_COUNT);

#define SIGNAL_BIT_(x) \
    NFC_CLIENT_BASE_SIGNAL_BIT(NFC_PEER_SERVICE_PROPERTY_##x)

#define nfc_peer_service_emit_queued_signals(self) \
    nfc_client_base_emit_queued_signals(&(self)->base)
#define nfc_peer_service_signal_property_change(self,NAME) \
    nfc_client_base_signal_property_change(&(self)->base, \
    NFC_PEER_SERVICE_PROPERTY_##NAME)
#define nfc_peer_service_queue_signal(self,property) \
    ((self)->base.queued_signals |= NFC_CLIENT_BASE_SIGNAL_BIT(property))
#define nfc_peer_service_queue_signal_(self,NAME) \
    ((self)->base.queued_signals |= SIGNAL_BIT_(NAME))

enum nfc_peer_service_signal {
    SIGNAL_PEER_ARRIVED,
    SIGNAL_PEER_LEFT,
    SIGNAL_ACCEPT,
    SIGNAL_COUNT
};

#define SIGNAL_PEER_ARRIVED_NAME      "nfcdc-peer-service-peer-arrived"
#define SIGNAL_PEER_LEFT_NAME         "nfcdc-peer-service-peer-left"
#define SIGNAL_ACCEPT_NAME            "nfcdc-peer-service-accept"

static guint nfc_peer_service_signals[SIGNAL_COUNT];

/*==========================================================================*
 * Implementation
 *==========================================================================*/

static inline
NfcPeerServiceObject*
nfc_peer_service_object_cast(
    NfcPeerService* service)
{
    return service ? THIS(G_CAST(service, NfcPeerServiceObject, pub)) : NULL;
}

static inline
NfcServiceConnectionPriv*
nfc_service_connection_cast(
    NfcServiceConnection* conn)
{
    return conn ? G_CAST(conn, NfcServiceConnectionPriv, pub) : NULL;
}

static
NfcServiceConnectionPriv*
nfc_service_connection_priv_new(
    OrgSailfishosNfcLocalService* object,
    GDBusMethodInvocation* call,
    GUnixFDList* fdl,
    guint rsap)
{
    NfcServiceConnectionPriv* priv = g_slice_new0(NfcServiceConnectionPriv);
    NfcServiceConnection* conn = &priv->pub;

    conn->rsap = rsap;
    conn->fd = g_unix_fd_list_get(fdl, 0, NULL);
    g_atomic_int_set(&priv->refcount, 1);
    g_object_ref(priv->object = object);
    g_object_ref(priv->accept_call = call);
    return priv;
}

static
void
nfc_service_connection_priv_unref(
    NfcServiceConnectionPriv* priv)
{
    if (g_atomic_int_dec_and_test(&priv->refcount)) {
        NfcServiceConnection* conn = &priv->pub;

        if (priv->accept_call) {
            /* Reject the connection */
            GDEBUG("Rejecting connection from %u", conn->rsap);
            org_sailfishos_nfc_local_service_complete_accept(priv->object,
                priv->accept_call, NULL, FALSE);
            g_object_unref(priv->accept_call);
        }
        g_object_unref(priv->object);
        shutdown(conn->fd, SHUT_RDWR);
        close(conn->fd);
        gutil_slice_free(conn);
    }
}

static
void
nfc_peer_service_try_register(
    NfcPeerServiceObject* self)
{
    NfcPeerService* service = &self->pub;

    if (!service->sap && self->exported) {
        NfcDaemonClient* daemon = self->daemon;

        if (daemon->valid && daemon->present) {
            nfc_daemon_client_register_service(daemon, service);
        }
    }
}

static
void
nfc_peer_service_daemon_presence_changed(
    NfcDaemonClient* daemon,
    NFC_DAEMON_PROPERTY property,
    void* user_data)
{
    NfcPeerServiceObject* self = THIS(user_data);
    NfcPeerService* service = &self->pub;

    if (daemon->present) {
        nfc_peer_service_try_register(self);
    } else if (service->sap) {
        service->sap = 0;
        nfc_peer_service_queue_signal_(self, SAP);
    }
    nfc_peer_service_emit_queued_signals(self);
}

static
gboolean
nfc_peer_service_object_handle_peer_arrived(
    OrgSailfishosNfcLocalService* object,
    GDBusMethodInvocation* call,
    const char* path,
    NfcPeerServiceObject* self)
{
    GDEBUG("Peer %s arrived", path);
    g_signal_emit(self, nfc_peer_service_signals [SIGNAL_PEER_ARRIVED], 0,
        path);
    org_sailfishos_nfc_local_service_complete_peer_arrived(object, call);
    return TRUE;
}

static
gboolean
nfc_peer_service_object_handle_peer_left(
    OrgSailfishosNfcLocalService* object,
    GDBusMethodInvocation* call,
    const char* path,
    NfcPeerServiceObject* self)
{
    GDEBUG("Peer %s left", path);
    g_signal_emit(self, nfc_peer_service_signals [SIGNAL_PEER_LEFT], 0, path);
    org_sailfishos_nfc_local_service_complete_peer_left(object, call);
    return TRUE;
}

static
gboolean
nfc_peer_service_object_handle_accept(
    OrgSailfishosNfcLocalService* object,
    GDBusMethodInvocation* call,
    GUnixFDList* fdl,
    guint rsap,
    GVariant* var,
    NfcPeerServiceObject* self)
{
    if (self->handler) {
        NfcServiceConnectionPriv* priv =
            nfc_service_connection_priv_new(object, call, fdl,rsap);

        self->handler(&self->pub, &priv->pub, self->handler_data);
        nfc_service_connection_priv_unref(priv);
    }
    return TRUE;
}

/*==========================================================================*
 * Internal API
 *==========================================================================*/

void
nfc_peer_service_registered(
    NfcPeerService* service,
    guint sap)
{
    NfcPeerServiceObject* self = nfc_peer_service_object_cast(service);

    GDEBUG("Service %s SAP %u", service->sn, sap);
    if (service->sap != sap) {
        service->sap = sap;
        nfc_peer_service_signal_property_change(self, SAP);
    }
}

void
nfc_peer_service_registeration_failed(
    NfcPeerService* service,
    const GError* error)
{
    NfcPeerServiceObject* self = nfc_peer_service_object_cast(service);

    GERR("Service %s registration error: %s", service->sn, GERRMSG(error));
    if (service->sap) {
        service->sap = 0;
        nfc_peer_service_signal_property_change(self, SAP);
    }
}

/*==========================================================================*
 * API
 *==========================================================================*/

NfcPeerService*
nfc_peer_service_new(
    const char* path,
    const char* sn,
    NfcPeerServiceHandlerFunc handler,
    void* handler_data)
{
    NfcPeerService* service = NULL;

    if (path && path[0] && handler) {
        NfcPeerServiceObject* self = g_object_new(THIS_TYPE, NULL);
        GError* error = NULL;

        service = &self->pub;
        if (sn) {
            service->sn = self->sn = g_strdup(sn);
        }
        service->path = self->path = g_strdup(path);
        self->handler = handler;
        self->handler_data = handler_data;

        self->daemon = nfc_daemon_client_new();
        self->daemon_event_id[DAEMON_PRESENT_CHANGED] =
            nfc_daemon_client_add_property_handler(self->daemon,
                NFC_DAEMON_PROPERTY_PRESENT,
                nfc_peer_service_daemon_presence_changed, self);

        self->object = org_sailfishos_nfc_local_service_skeleton_new();
        self->call_id[CALL_ACCEPT] =
            g_signal_connect(self->object, "handle-accept",
                G_CALLBACK(nfc_peer_service_object_handle_accept), self);
        self->call_id[CALL_PEER_ARRIVED] =
            g_signal_connect(self->object, "handle-peer-arrived",
                G_CALLBACK(nfc_peer_service_object_handle_peer_arrived), self);
        self->call_id[CALL_PEER_LEFT] =
            g_signal_connect(self->object, "handle-peer-left",
                G_CALLBACK(nfc_peer_service_object_handle_peer_left), self);

        self->exported = g_dbus_interface_skeleton_export
            (G_DBUS_INTERFACE_SKELETON(self->object),
                nfc_daemon_client_connection(self->daemon), path, &error);

        if (self->exported) {
            GDEBUG("Exported %s", path);
            nfc_peer_service_try_register(self);
        } else {
            GERR("%s", GERRMSG(error));
            g_error_free(error);
        }
    }
    return service;
}

NfcPeerService*
nfc_peer_service_ref(
    NfcPeerService* service)
{
    if (G_LIKELY(service)) {
        g_object_ref(nfc_peer_service_object_cast(service));
        return service;
    } else {
        return NULL;
    }
}

void
nfc_peer_service_unref(
    NfcPeerService* service)
{
    if (G_LIKELY(service)) {
        g_object_unref(nfc_peer_service_object_cast(service));
    }
}

gulong
nfc_peer_service_add_property_handler(
    NfcPeerService* service,
    NFC_PEER_SERVICE_PROPERTY property,
    NfcPeerServicePropertyFunc func,
    void* user_data)
{
    NfcPeerServiceObject* self = nfc_peer_service_object_cast(service);

    return G_LIKELY(self) ? nfc_client_base_add_property_handler(&self->base,
        property, (NfcClientBasePropertyFunc) func, user_data) : 0;
}

gulong
nfc_peer_service_add_peer_arrived_handler(
    NfcPeerService* service,
    NfcPeerServicePathFunc func,
    void* user_data)
{
    NfcPeerServiceObject* self = nfc_peer_service_object_cast(service);

    return (G_LIKELY(self) && G_LIKELY(func)) ? g_signal_connect(self,
        SIGNAL_PEER_ARRIVED_NAME, G_CALLBACK(func), user_data) : 0;
}

gulong
nfc_peer_service_add_peer_left_handler(
    NfcPeerService* service,
    NfcPeerServicePathFunc func,
    void* user_data)
{
    NfcPeerServiceObject* self = nfc_peer_service_object_cast(service);

    return (G_LIKELY(self) && G_LIKELY(func)) ? g_signal_connect(self,
        SIGNAL_PEER_LEFT_NAME, G_CALLBACK(func), user_data) : 0;
}

void
nfc_peer_service_remove_handler(
    NfcPeerService* service,
    gulong id)
{
    if (G_LIKELY(id)) {
        NfcPeerServiceObject* self = nfc_peer_service_object_cast(service);

        if (G_LIKELY(self)) {
            g_signal_handler_disconnect(self, id);
        }
    }
}

void
nfc_peer_service_remove_handlers(
    NfcPeerService* service,
    gulong* ids,
    guint n)
{
    gutil_disconnect_handlers(nfc_peer_service_object_cast(service), ids, n);
}

NfcServiceConnection*
nfc_service_connection_ref(
    NfcServiceConnection* conn)
{
    NfcServiceConnectionPriv* priv = nfc_service_connection_cast(conn);

    if (priv) {
        GASSERT(priv->refcount > 0);
        g_atomic_int_inc(&priv->refcount);
    }
    return conn;
}

void
nfc_service_connection_unref(
    NfcServiceConnection* conn)
{
    NfcServiceConnectionPriv* priv = nfc_service_connection_cast(conn);

    if (priv) {
        nfc_service_connection_priv_unref(priv);
    }
}

NfcServiceConnection*
nfc_service_connection_accept(
    NfcServiceConnection* conn)
{
    NfcServiceConnectionPriv* priv = nfc_service_connection_cast(conn);

    if (priv && priv->accept_call) {
        GDEBUG("Accepting connection from %u", conn->rsap);
        org_sailfishos_nfc_local_service_complete_accept(priv->object,
            priv->accept_call, NULL, TRUE);
        g_object_unref(priv->accept_call);
        priv->accept_call = NULL;
        /* Add a reference */
        g_atomic_int_inc(&priv->refcount);
        return conn;
    }
    /* Already accepted, don't increment the refcount */
    return NULL;
}

/*==========================================================================*
 * Internals
 *==========================================================================*/

static
void
nfc_peer_service_object_init(
    NfcPeerServiceObject* self)
{
}

static
void
nfc_peer_service_object_finalize(
    GObject* object)
{
    NfcPeerServiceObject* self = THIS(object);

    GVERBOSE_("%s", self->path);
    if (self->exported) {
        nfc_daemon_client_unregister_service(self->daemon, self->path);
        g_dbus_interface_skeleton_unexport
            (G_DBUS_INTERFACE_SKELETON(self->object));
    }
    gutil_disconnect_handlers(self->object, self->call_id, CALL_COUNT);
    g_object_unref(self->object);
    g_free(self->path);
    g_free(self->sn);
    nfc_daemon_client_remove_all_handlers(self->daemon, self->daemon_event_id);
    nfc_daemon_client_unref(self->daemon);
    G_OBJECT_CLASS(PARENT_CLASS)->finalize(object);
}

static
void
nfc_peer_service_object_class_init(
    NfcPeerServiceObjectClass* klass)
{
    GType type = G_OBJECT_CLASS_TYPE(klass);

    G_OBJECT_CLASS(klass)->finalize = nfc_peer_service_object_finalize;
    klass->public_offset = G_STRUCT_OFFSET(NfcPeerServiceObject, pub);

    nfc_peer_service_signals[SIGNAL_PEER_ARRIVED] =
        g_signal_new(SIGNAL_PEER_ARRIVED_NAME, type,
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
            G_TYPE_STRING);
    nfc_peer_service_signals[SIGNAL_PEER_LEFT] =
        g_signal_new(SIGNAL_PEER_LEFT_NAME, type,
            G_SIGNAL_RUN_FIRST, 0, NULL, NULL, NULL, G_TYPE_NONE, 1,
            G_TYPE_STRING);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
