/*
 * Copyright (C) 2019-2021 Jolla Ltd.
 * Copyright (C) 2019-2021 Slava Monich <slava@monich.com>
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

#ifndef NFCDC_ADAPTER_H
#define NFCDC_ADAPTER_H

#include <nfcdc_types.h>

G_BEGIN_DECLS

typedef enum nfc_adapter_property {
    NFC_ADAPTER_PROPERTY_ANY,
    NFC_ADAPTER_PROPERTY_VALID,
    NFC_ADAPTER_PROPERTY_PRESENT,
    NFC_ADAPTER_PROPERTY_ENABLED,
    NFC_ADAPTER_PROPERTY_POWERED,
    NFC_ADAPTER_PROPERTY_MODE,
    NFC_ADAPTER_PROPERTY_TARGET_PRESENT,
    NFC_ADAPTER_PROPERTY_TAGS,
    /* Since 1.0.6 */
    NFC_ADAPTER_PROPERTY_PEERS,
    /* Moving target: */
    NFC_ADAPTER_PROPERTY_COUNT
} NFC_ADAPTER_PROPERTY;

/* NFC_ADAPTER_MODE was replaced with NFC_MODE in 1.0.6 */
#define NFC_ADAPTER_MODE                NFC_MODE
#define NFC_ADAPTER_MODE_NONE           NFC_MODE_NONE
#define NFC_ADAPTER_MODE_P2P_INITIATOR  NFC_MODE_P2P_INITIATOR
#define NFC_ADAPTER_MODE_READER_WRITER  NFC_MODE_READER_WRITER
#define NFC_ADAPTER_MODE_P2P_TARGET     NFC_MODE_P2P_TARGET
#define NFC_ADAPTER_MODE_CARD_EMILATION NFC_MODE_CARD_EMILATION

struct nfc_adapter_client {
    const char* path;
    gboolean valid;
    gboolean present;
    gboolean enabled;
    gboolean powered;
    NFC_MODE supported_modes;
    NFC_MODE mode;
    gboolean target_present;
    const GStrV* tags;
    /* Since 1.0.6 */
    const GStrV* peers;
};

typedef
void
(*NfcAdapterPropertyFunc)(
    NfcAdapterClient* adapter,
    NFC_ADAPTER_PROPERTY property,
    void* user_data);

NfcAdapterClient*
nfc_adapter_client_new(
    const char* path);

NfcAdapterClient*
nfc_adapter_client_ref(
    NfcAdapterClient* adapter);

void
nfc_adapter_client_unref(
    NfcAdapterClient* adapter);

gulong
nfc_adapter_client_add_property_handler(
    NfcAdapterClient* adapter,
    NFC_ADAPTER_PROPERTY property,
    NfcAdapterPropertyFunc callback,
    void* user_data);

void
nfc_adapter_client_remove_handler(
    NfcAdapterClient* adapter,
    gulong id);

void
nfc_adapter_client_remove_handlers(
    NfcAdapterClient* adapter,
    gulong* ids,
    guint count);

#define nfc_adapter_client_remove_all_handlers(adapter, ids) \
    nfc_adapter_client_remove_handlers(adapter, ids, G_N_ELEMENTS(ids))

G_END_DECLS

#endif /* NFCDC_ADAPTER_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
