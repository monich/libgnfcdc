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

#ifndef NFCDC_TAG_H
#define NFCDC_TAG_H

#include <nfcdc_types.h>

#include <gio/gio.h>

G_BEGIN_DECLS

#define NFC_TAG_INTERFACE_ISODEP "org.sailfishos.nfc.IsoDep"
#define NFC_TAG_INTERFACE_TYPE2  "org.sailfishos.nfc.TagType2"

typedef enum nfc_tag_property {
    NFC_TAG_PROPERTY_ANY,
    NFC_TAG_PROPERTY_VALID,
    NFC_TAG_PROPERTY_PRESENT,
    NFC_TAG_PROPERTY_INTERFACES,
    NFC_TAG_PROPERTY_NDEF_RECORDS,
    NFC_TAG_PROPERTY_COUNT
} NFC_TAG_PROPERTY;

typedef enum nfc_tag_poll_param {
    /* NFC-A */
    NFC_TAG_POLL_PARAM_SELRES,    /* SEL_RES Response */
    NFC_TAG_POLL_PARAM_NFCID1,    /* NFCID1 */
    /* NFC-B */
    NFC_TAG_POLL_PARAM_NFCID0,    /* NFCID0 (optional) */
    NFC_TAG_POLL_PARAM_APPDATA,   /* SENSB_RES */
    NFC_TAG_POLL_PARAM_PROTINFO,  /* SENSB_RES/PROTINFO (optional) */
    NFC_TAG_POLL_PARAM_COUNT
} NFC_TAG_POLL_PARAM; /* Since 1.0.10 */

struct nfc_tag_client {
    const char* path;
    gboolean valid;
    gboolean present;
    const GStrV* interfaces;
    const GStrV* ndef_records;
};

typedef struct nfc_tag_client_lock NfcTagClientLock;

typedef
void
(*NfcTagClientCallFunc)(
    NfcTagClient* tag,
    const GError* error,
    void* user_data); /* Since 1.0.9 */

typedef
void
(*NfcTagPropertyFunc)(
    NfcTagClient* tag,
    NFC_TAG_PROPERTY property,
    void* user_data);

typedef
void
(*NfcTagClientLockFunc)(
    NfcTagClient* tag,
    NfcTagClientLock* lock,
    const GError* error,
    void* user_data);

NfcTagClient*
nfc_tag_client_new(
    const char* path);

NfcTagClient*
nfc_tag_client_ref(
    NfcTagClient* tag);

void
nfc_tag_client_unref(
    NfcTagClient* tag);

NfcTagClientLock*
nfc_tag_client_get_lock(
    NfcTagClient* tag); /* Since 1.0.10 */

gboolean
nfc_tag_client_acquire_lock(
    NfcTagClient* tag,
    gboolean wait,
    GCancellable* cancel,
    NfcTagClientLockFunc callback,
    void* user_data,
    GDestroyNotify destroy);

const GUtilData*
nfc_tag_client_poll_param(
    NfcTagClient* tag,
    NFC_TAG_POLL_PARAM param); /* Since 1.0.10 */

gboolean
nfc_tag_client_deactivate(
    NfcTagClient* tag,
    GCancellable* cancel,
    NfcTagClientCallFunc callback,
    void* user_data,
    GDestroyNotify destroy); /* Since 1.0.9 */

gulong
nfc_tag_client_add_property_handler(
    NfcTagClient* tag,
    NFC_TAG_PROPERTY property,
    NfcTagPropertyFunc callback,
    void* user_data);

void
nfc_tag_client_remove_handler(
    NfcTagClient* tag,
    gulong id);

void
nfc_tag_client_remove_handlers(
    NfcTagClient* tag,
    gulong* ids,
    guint count);

#define nfc_tag_client_remove_all_handlers(tag, ids) \
    nfc_tag_client_remove_handlers(tag, ids, G_N_ELEMENTS(ids))

NfcTagClientLock*
nfc_tag_client_lock_ref(
    NfcTagClientLock* lock);

void
nfc_tag_client_lock_unref(
    NfcTagClientLock* lock);

G_END_DECLS

#endif /* NFCDC_TAG_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
