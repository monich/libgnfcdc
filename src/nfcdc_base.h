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

#ifndef NFCDC_BASE_H
#define NFCDC_BASE_H

#include "nfcdc_types.h"

#include <glib-object.h>

typedef struct nfc_client_base_class {
    GObjectClass object;
    int public_offset;
    int valid_offset;
} NfcClientBaseClass;

typedef struct nfc_client_base {
    GObject object;
    guint32 queued_signals;
} NfcClientBase;

G_GNUC_INTERNAL GType nfc_client_base_get_type(void);
#define NFC_CLIENT_TYPE_BASE (nfc_client_base_get_type())

typedef enum nfc_client_base_property {
    NFC_CLIENT_BASE_PROPERTY_ANY = 0,
    NFC_CLIENT_BASE_PROPERTY_VALID,
    NFC_CLIENT_BASE_MAX_PROPERTIES = 32
} NFC_CLIENT_BASE_PROPERTY;

#define NFC_CLIENT_BASE_ASSERT_VALID(valid) \
    G_STATIC_ASSERT((int)(valid) == (int)NFC_CLIENT_BASE_PROPERTY_VALID)
#define NFC_CLIENT_BASE_ASSERT_COUNT(count) \
    G_STATIC_ASSERT((int)count <= (int)NFC_CLIENT_BASE_MAX_PROPERTIES)
#define NFC_CLIENT_BASE_SIGNAL_BIT(property) (1 << (property - 1))

typedef
void
(*NfcClientBasePropertyFunc)(
    gpointer source,
    guint property,
    gpointer user_data);

gulong
nfc_client_base_add_property_handler(
    NfcClientBase* base,
    guint property,
    NfcClientBasePropertyFunc callback,
    gpointer user_data)
    G_GNUC_INTERNAL;

void
nfc_client_base_emit_queued_signals(
    NfcClientBase* base)
    G_GNUC_INTERNAL;

void
nfc_client_base_signal_property_change(
    NfcClientBase* self,
    guint property)
    G_GNUC_INTERNAL;

#endif /* NFCDC_BASE_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
