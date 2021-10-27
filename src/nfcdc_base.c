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

#include "nfcdc_base.h"
#include "nfcdc_log.h"

G_DEFINE_ABSTRACT_TYPE(NfcClientBase, nfc_client_base, G_TYPE_OBJECT)
#define NFC_CLIENT_BASE_GET_CLASS(obj) G_TYPE_INSTANCE_GET_CLASS((obj), \
        NFC_CLIENT_TYPE_BASE, NfcClientBaseClass)

typedef struct nfc_client_base_closure {
    GCClosure cclosure;
    NfcClientBasePropertyFunc callback;
    gpointer user_data;
} NfcClientBaseClosure;

#define nfc_client_base_closure_new() ((NfcClientBaseClosure *) \
    g_closure_new_simple(sizeof(NfcClientBaseClosure), NULL))

#define SIGNAL_PROPERTY_CHANGED_NAME    "nfcdc-base-property-changed"
#define SIGNAL_PROPERTY_DETAIL          "%x"
#define SIGNAL_PROPERTY_DETAIL_MAX_LEN  (8)

#define SIGNAL_BIT_(name) \
    NFC_CLIENT_BASE_SIGNAL_BIT(NFC_CLIENT_BASE_PROPERTY_##name)

enum nfc_client_base_signal {
    SIGNAL_PROPERTY_CHANGED,
    SIGNAL_COUNT
};

static guint nfc_client_base_signals[SIGNAL_COUNT];
static GQuark nfc_client_base_property_quarks[NFC_CLIENT_BASE_MAX_PROPERTIES];

static
GQuark
nfc_client_base_property_quark(
    guint property)
{
    GASSERT(property < NFC_CLIENT_BASE_MAX_PROPERTIES);
    /* For ANY property this function is expected to return zero */
    if (property > 0 && G_LIKELY(property < NFC_CLIENT_BASE_MAX_PROPERTIES)) {
        const int i = property - 1;

        if (G_UNLIKELY(!nfc_client_base_property_quarks[i])) {
            char buf[SIGNAL_PROPERTY_DETAIL_MAX_LEN + 1];

            snprintf(buf, sizeof(buf), SIGNAL_PROPERTY_DETAIL, property);
            buf[sizeof(buf) - 1] = 0;
            nfc_client_base_property_quarks[i] = g_quark_from_string(buf);
        }
        return nfc_client_base_property_quarks[i];
    }
    return 0;
}

static
void
nfc_client_base_property_changed(
    NfcClientBase* self,
    guint property,
    NfcClientBaseClosure* closure)
{
    const NfcClientBaseClass* klass = NFC_CLIENT_BASE_GET_CLASS(self);

    closure->callback(((guint8*)self) + klass->public_offset, property,
        closure->user_data);
}

static
void
nfc_client_base_emit_property_changed_signal(
    NfcClientBase* self,
    guint property)
{
    self->queued_signals &= ~NFC_CLIENT_BASE_SIGNAL_BIT(property);
    g_signal_emit(self, nfc_client_base_signals[SIGNAL_PROPERTY_CHANGED],
        nfc_client_base_property_quark(property), property);
}

void
nfc_client_base_signal_property_change(
    NfcClientBase* self,
    guint property)
{
    /* N.B. This may signal more than one property change */
    self->queued_signals |= NFC_CLIENT_BASE_SIGNAL_BIT(property);
    nfc_client_base_emit_queued_signals(self);
}

void
nfc_client_base_emit_queued_signals(
    NfcClientBase* self)
{
    const NfcClientBaseClass* klass = NFC_CLIENT_BASE_GET_CLASS(self);
    const guint8* base = (guint8*)self;
    const gboolean* valid = (gboolean*)(klass->valid_offset ?
        (base + klass->valid_offset) : NULL);
    gboolean valid_changed;
    guint p;

    /* Handlers could drop their references to us */
    g_object_ref(self);

    /* VALID is the last signals to be emitted if the object BECOMES valid */
    if (valid && (*valid) && (self->queued_signals & SIGNAL_BIT_(VALID))) {
        self->queued_signals &= ~SIGNAL_BIT_(VALID);
        valid_changed = TRUE;
    } else {
        valid_changed = FALSE;
    }

    /*
     * Emit the signals. Not that in case if valid has become FALSE,
     * then VALID is emitted first, otherwise it's emitted last.
     */
    for (p = valid ? 1 : 0; /* Skip VALID if we have it */
         self->queued_signals && p < NFC_CLIENT_BASE_MAX_PROPERTIES;
         p++) {
        if (self->queued_signals & NFC_CLIENT_BASE_SIGNAL_BIT(p)) {
            nfc_client_base_emit_property_changed_signal(self, p);
        }
    }

    /* Then emit VALID if necessary */
    if (valid_changed) {
        nfc_client_base_emit_property_changed_signal(self,
            NFC_CLIENT_BASE_PROPERTY_VALID);
    }

    /* And release the temporary reference */
    g_object_unref(self);
}

gulong
nfc_client_base_add_property_handler(
    NfcClientBase* self,
    guint property,
    NfcClientBasePropertyFunc callback,
    gpointer user_data)
{
    if (G_LIKELY(callback)) {
        /*
         * We can't directly connect the provided callback because
         * it expects the first parameter to point to public part
         * of the object but glib will call it with NfcClientBase as
         * the first parameter. nfc_client_base_property_changed() will
         * do the conversion.
         */
        NfcClientBaseClosure* closure = nfc_client_base_closure_new();
        GCClosure* cc = &closure->cclosure;

        cc->closure.data = closure;
        cc->callback = G_CALLBACK(nfc_client_base_property_changed);
        closure->callback = callback;
        closure->user_data = user_data;

        return g_signal_connect_closure_by_id(self,
            nfc_client_base_signals[SIGNAL_PROPERTY_CHANGED],
            nfc_client_base_property_quark(property), &cc->closure, FALSE);
    }
    return 0;
}

static
void
nfc_client_base_init(
    NfcClientBase* self)
{
}

static
void
nfc_client_base_class_init(
    NfcClientBaseClass* klass)
{
    nfc_client_base_signals[SIGNAL_PROPERTY_CHANGED] =
        g_signal_new(SIGNAL_PROPERTY_CHANGED_NAME, G_OBJECT_CLASS_TYPE(klass),
            G_SIGNAL_RUN_FIRST | G_SIGNAL_DETAILED, 0, NULL, NULL, NULL,
            G_TYPE_NONE, 1, G_TYPE_UINT);
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
