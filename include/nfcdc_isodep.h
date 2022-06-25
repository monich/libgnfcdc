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

#ifndef NFCDC_ISODEP_H
#define NFCDC_ISODEP_H

#include <nfcdc_types.h>

#include <gio/gio.h>

#define NFC_ISODEP_SW1(sw) (((sw) >> 8) & 0xff)
#define NFC_ISODEP_SW2(sw) ((sw) & 0xff)
#define NFC_ISODEP_SW(sw1,sw2) ((((sw1) << 8) & 0xff00) | ((sw2) & 0xff))
#define NFC_ISODEP_SW_OK NFC_ISODEP_SW(0x90,00) /* Normal completion */

G_BEGIN_DECLS

typedef enum nfc_isodep_property {
    NFC_ISODEP_PROPERTY_ANY,
    NFC_ISODEP_PROPERTY_VALID,
    NFC_ISODEP_PROPERTY_PRESENT,
    NFC_ISODEP_PROPERTY_COUNT
} NFC_ISODEP_PROPERTY;

typedef enum nfc_isodep_act_param {
    /* NFC-A */
    NFC_ISODEP_ACT_PARAM_T0,    /* Format Byte T0 */
    NFC_ISODEP_ACT_PARAM_TA,    /* Interface Bytes TA (optional) */
    NFC_ISODEP_ACT_PARAM_TB,    /* Interface Bytes TB (optional) */
    NFC_ISODEP_ACT_PARAM_TC,    /* Interface Bytes TC (optional) */
    NFC_ISODEP_ACT_PARAM_HB,    /* Historical Bytes */
    /* NFC-B */
    NFC_ISODEP_ACT_PARAM_MBLI,  /* Maximum Buffer Length Index */
    NFC_ISODEP_ACT_PARAM_DID,   /* Device ID */
    NFC_ISODEP_ACT_PARAM_HLR,   /* Higher Layer Response (optional) */
    NFC_ISODEP_ACT_PARAM_COUNT
} NFC_ISODEP_ACT_PARAM;

struct nfc_isodep_client {
    const char* path;
    gboolean valid;
    gboolean present;
};

struct nfc_isodep_apdu {
    guint8 cla;     /* Class byte */
    guint8 ins;     /* Instruction byte */
    guint8 p1;      /* Parameter byte 1 */
    guint8 p2;      /* Parameter byte 2 */
    GUtilData data; /* Command data */
    guint le;       /* Expected length, zero if none */
};

typedef
void
(*NfcIsoDepPropertyFunc)(
    NfcIsoDepClient* isodep,
    NFC_ISODEP_PROPERTY property,
    void* user_data);

typedef
void
(*NfcIsoDepTransmitFunc)(
    NfcIsoDepClient* isodep,
    const GUtilData* response,
    guint sw,  /* 16 bits (SW1 << 8)|SW2 */
    const GError* error,
    void* user_data);

NfcIsoDepClient*
nfc_isodep_client_new(
    const char* path);

NfcIsoDepClient*
nfc_isodep_client_ref(
    NfcIsoDepClient* isodep);

void
nfc_isodep_client_unref(
    NfcIsoDepClient* isodep);

NfcTagClient*
nfc_isodep_client_tag(
    NfcIsoDepClient* isodep); /* Since 1.0.10 */

const GUtilData*
nfc_isodep_client_act_param(
    NfcIsoDepClient* isodep,
    NFC_ISODEP_ACT_PARAM param); /* Since 1.0.8 */

gboolean
nfc_isodep_client_transmit(
    NfcIsoDepClient* isodep,
    const NfcIsoDepApdu* apdu,
    GCancellable* cancel,
    NfcIsoDepTransmitFunc callback,
    void* user_data,
    GDestroyNotify destroy);

gulong
nfc_isodep_client_add_property_handler(
    NfcIsoDepClient* isodep,
    NFC_ISODEP_PROPERTY property,
    NfcIsoDepPropertyFunc callback,
    void* user_data);

void
nfc_isodep_client_remove_handler(
    NfcIsoDepClient* isodep,
    gulong id);

void
nfc_isodep_client_remove_handlers(
    NfcIsoDepClient* isodep,
    gulong* ids,
    guint count);

#define nfc_isodep_client_remove_all_handlers(isodep, ids) \
    nfc_isodep_client_remove_handlers(isodep, ids, G_N_ELEMENTS(ids))

G_END_DECLS

#endif /* NFCDC_ISODEP_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
