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

#ifndef NFCDC_PEER_SERVICE_H
#define NFCDC_PEER_SERVICE_H

#include <nfcdc_types.h>

/* This API exists since 1.0.6 */

G_BEGIN_DECLS

typedef enum nfc_peer_service_property {
    NFC_PEER_SERVICE_PROPERTY_ANY,
    NFC_PEER_SERVICE_PROPERTY_SAP,
    NFC_PEER_SERVICE_PROPERTY_COUNT
} NFC_PEER_SERVICE_PROPERTY;

struct nfc_peer_service {
    const char* path;
    const char* sn;
    guint sap;
};

typedef
void
(*NfcPeerServiceHandlerFunc)(
    NfcPeerService* service,
    NfcServiceConnection* connection,
    void* user_data);

typedef
void
(*NfcPeerServicePropertyFunc)(
    NfcPeerService* service,
    NFC_PEER_SERVICE_PROPERTY property,
    void* user_data);

typedef
void
(*NfcPeerServicePathFunc)(
    NfcPeerService* service,
    const char* path,
    void* user_data);

NfcPeerService*
nfc_peer_service_new(
    const char* path,
    const char* sn,
    NfcPeerServiceHandlerFunc handler,
    void* user_data);

NfcPeerService*
nfc_peer_service_ref(
    NfcPeerService* service);

void
nfc_peer_service_unref(
    NfcPeerService* service);

gulong
nfc_peer_service_add_property_handler(
    NfcPeerService* service,
    NFC_PEER_SERVICE_PROPERTY property,
    NfcPeerServicePropertyFunc func,
    void* user_data);

gulong
nfc_peer_service_add_peer_arrived_handler(
    NfcPeerService* service,
    NfcPeerServicePathFunc func,
    void* user_data);

gulong
nfc_peer_service_add_peer_left_handler(
    NfcPeerService* service,
    NfcPeerServicePathFunc func,
    void* user_data);

void
nfc_peer_service_remove_handler(
    NfcPeerService* service,
    gulong id);

void
nfc_peer_service_remove_handlers(
    NfcPeerService* service,
    gulong* ids,
    guint count);

#define nfc_peer_service_remove_all_handlers(service, ids) \
    nfc_peer_service_remove_handlers(service, ids, G_N_ELEMENTS(ids))

/*
 * Incoming connection.
 *
 * Passed to NfcPeerServiceHandlerFunc, must be accepted by
 * nfc_service_connection_accept() which also adds a reference
 * to the connection. That reference has to be eventauly dropped
 * with nfc_service_connection_unref().
 *
 * Second, or any subsequent nfc_service_connection_accept() call
 * returns NULL and doesn't change the refcount.
 *
 * Not doing anything is equivalent to rejecting the connection.
 * File descriptor is owned by NfcServiceConnection and closed
 * when connection is finalized (ref count drops to zero).
 */

struct nfc_service_connection {
    int fd;
    guint rsap;
};

NfcServiceConnection*
nfc_service_connection_ref(
    NfcServiceConnection* conn);

void
nfc_service_connection_unref(
    NfcServiceConnection* conn);

NfcServiceConnection*
nfc_service_connection_accept(
    NfcServiceConnection* conn);

G_END_DECLS

#endif /* NFCDC_PEER_SERVICE_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
