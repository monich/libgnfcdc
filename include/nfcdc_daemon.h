/*
 * Copyright (C) 2019-2024 Slava Monich <slava@monich.com>
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

#ifndef NFCDC_DAEMON_H
#define NFCDC_DAEMON_H

#include <nfcdc_types.h>

#include <gio/gio.h>

G_BEGIN_DECLS

typedef enum nfc_daemon_property {
    NFC_DAEMON_PROPERTY_ANY,
    NFC_DAEMON_PROPERTY_VALID,
    NFC_DAEMON_PROPERTY_PRESENT,
    NFC_DAEMON_PROPERTY_ERROR,
    NFC_DAEMON_PROPERTY_ENABLED,
    NFC_DAEMON_PROPERTY_ADAPTERS,
    /* Since 1.0.3 */
    NFC_DAEMON_PROPERTY_VERSION,
    /* Since 1.0.6 */
    NFC_DAEMON_PROPERTY_MODE,
    /* Since 1.2.0 */
    NFC_DAEMON_PROPERTY_TECHS,
    NFC_DAEMON_PROPERTY_COUNT /* Moving target */
} NFC_DAEMON_PROPERTY;

struct nfc_daemon_client {
    gboolean valid;
    gboolean present;
    gboolean enabled;
    const GError* error;
    const GStrV* adapters;
    /* Since 1.0.3 */
    int version; /* Zero for nfcd versions < 1.0.26 */
    /* Since 1.0.6 */
    NFC_MODE mode; /* Zero for nfcd versions < 1.1.0 */
    /* Since 1.2.0 */
    NFC_TECH techs; /* Zero for nfcd versions < 1.2.0 */
};

#define NFC_DAEMON_VERSION(v1,v2,v3) \
    ((((v1) & 0x7f) << 24) | \
     (((v2) & 0xfff) << 12) | \
      ((v3) & 0xfff))

#define NFC_DAEMON_VERSION_MAJOR(v)  (((v) >> 24) & 0x7f)
#define NFC_DAEMON_VERSION_MINOR(v)  (((v) >> 12) & 0xfff)
#define NFC_DAEMON_VERSION_RELEASE(v) ((v) & 0xfff)

/* RELEASE used to be called NANO before 1.2.0 */
#define NFC_DAEMON_VERSION_NANO(v)  NFC_DAEMON_VERSION_RELEASE(v)

typedef
void
(*NfcDaemonClientCallFunc)(
    NfcDaemonClient* daemon,
    const GError* error,
    void* user_data); /* Since 1.2.0 */

typedef
void
(*NfcDaemonPropertyFunc)(
    NfcDaemonClient* daemon,
    NFC_DAEMON_PROPERTY property,
    void* user_data);

NfcDaemonClient*
nfc_daemon_client_new(
    void);

NfcDaemonClient*
nfc_daemon_client_ref(
    NfcDaemonClient* daemon);

void
nfc_daemon_client_unref(
    NfcDaemonClient* daemon);

gboolean
nfc_daemon_client_register_local_host_service(
    NfcDaemonClient* daemon,
    const char* path,
    const char* name,
    GCancellable* cancel,
    NfcDaemonClientCallFunc callback,
    void* user_data,
    GDestroyNotify destroy); /* Since 1.2.0 */

gboolean
nfc_daemon_client_unregister_local_host_service(
    NfcDaemonClient* daemon,
    const char* path,
    GCancellable* cancel,
    NfcDaemonClientCallFunc callback,
    void* user_data,
    GDestroyNotify destroy); /* Since 1.2.0 */

gulong
nfc_daemon_client_add_property_handler(
    NfcDaemonClient* daemon,
    NFC_DAEMON_PROPERTY property,
    NfcDaemonPropertyFunc callback,
    void* user_data);

void
nfc_daemon_client_remove_handler(
    NfcDaemonClient* daemon,
    gulong id);

void
nfc_daemon_client_remove_handlers(
    NfcDaemonClient* daemon,
    gulong* ids,
    guint count);

#define nfc_daemon_client_remove_all_handlers(daemon, ids) \
    nfc_daemon_client_remove_handlers(daemon, ids, G_N_ELEMENTS(ids))

/* NfcModeRequest */

struct nfc_mode_request {
    NFC_MODE enable;
    NFC_MODE disable;
};  /* Since 1.0.6 */

NfcModeRequest*
nfc_mode_request_new(
    NfcDaemonClient* daemon,
    NFC_MODE enable,
    NFC_MODE disable);  /* Since 1.0.6 */

void
nfc_mode_request_free(
    NfcModeRequest* request);  /* Since 1.0.6 */

/* NfcTechRequest */

struct nfc_tech_request {
    NFC_TECH allow;
    NFC_TECH disallow;
};  /* Since 1.2.0 */

NfcTechRequest*
nfc_tech_request_new(
    NfcDaemonClient* daemon,
    NFC_TECH allow,
    NFC_TECH disallow);  /* Since 1.2.0 */

void
nfc_tech_request_free(
    NfcTechRequest* request);  /* Since 1.2.0 */

G_END_DECLS

#endif /* NFCDC_DAEMON_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
