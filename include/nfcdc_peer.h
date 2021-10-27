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

#ifndef NFCDC_PEER_H
#define NFCDC_PEER_H

#include <nfcdc_types.h>

#include <gio/gio.h>

/* This API exists since 1.0.6 */

G_BEGIN_DECLS

typedef struct nfc_peer_client_connection NfcPeerClientConnection;

typedef enum nfc_peer_property {
    NFC_PEER_PROPERTY_ANY,
    NFC_PEER_PROPERTY_VALID,
    NFC_PEER_PROPERTY_PRESENT,
    NFC_PEER_PROPERTY_WKS,
    NFC_PEER_PROPERTY_COUNT
} NFC_PEER_PROPERTY;

struct nfc_peer_client {
    const char* path;
    gboolean valid;
    gboolean present;
    guint wks;
};

typedef
void
(*NfcPeerPropertyFunc)(
    NfcPeerClient* peer,
    NFC_PEER_PROPERTY property,
    void* user_data);

typedef
void
(*NfcPeerClientConnectionFunc)(
    NfcPeerClient* peer,
    int fd,
    const GError* error,
    void* user_data);

NfcPeerClient*
nfc_peer_client_new(
    const char* path);

NfcPeerClient*
nfc_peer_client_ref(
    NfcPeerClient* peer);

void
nfc_peer_client_unref(
    NfcPeerClient* peer);

gboolean
nfc_peer_client_connect_sap(
    NfcPeerClient* peer,
    guint rsap,
    GCancellable* cancel,
    NfcPeerClientConnectionFunc callback,
    void* user_data,
    GDestroyNotify destroy);

gboolean
nfc_peer_client_connect_sn(
    NfcPeerClient* peer,
    const char* sn,
    GCancellable* cancel,
    NfcPeerClientConnectionFunc callback,
    void* user_data,
    GDestroyNotify destroy);

gulong
nfc_peer_client_add_property_handler(
    NfcPeerClient* peer,
    NFC_PEER_PROPERTY property,
    NfcPeerPropertyFunc callback,
    void* user_data);

void
nfc_peer_client_remove_handler(
    NfcPeerClient* peer,
    gulong id);

void
nfc_peer_client_remove_handlers(
    NfcPeerClient* peer,
    gulong* ids,
    guint count);

#define nfc_peer_client_remove_all_handlers(peer, ids) \
    nfc_peer_client_remove_handlers(peer, ids, G_N_ELEMENTS(ids))

G_END_DECLS

#endif /* NFCDC_PEER_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
