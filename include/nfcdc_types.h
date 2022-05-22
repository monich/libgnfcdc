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

#ifndef NFCDC_TYPES_H
#define NFCDC_TYPES_H

#include <gutil_types.h>

G_BEGIN_DECLS

#define NFCDC_LOG_MODULE nfcdc_log

typedef struct nfc_adapter_client NfcAdapterClient;
typedef struct nfc_daemon_client NfcDaemonClient;
typedef struct nfc_default_adapter NfcDefaultAdapter;
typedef struct nfc_isodep_apdu NfcIsoDepApdu;
typedef struct nfc_isodep_client NfcIsoDepClient;
typedef struct nfc_mode_request NfcModeRequest; /* Since 1.0.6 */
typedef struct nfc_service_connection NfcServiceConnection;  /* Since 1.0.6 */
typedef struct nfc_peer_client NfcPeerClient; /* Since 1.0.6 */
typedef struct nfc_peer_service NfcPeerService; /* Since 1.0.6 */
typedef struct nfc_tag_client NfcTagClient;

typedef enum nfc_daemon_mode {
    NFC_MODE_NONE           = 0x00,
    /* Polling */
    NFC_MODE_P2P_INITIATOR  = 0x01,
    NFC_MODE_READER_WRITER  = 0x02,
    /* Listening */
    NFC_MODE_P2P_TARGET     = 0x04,
    NFC_MODE_CARD_EMILATION = 0x08
} NFC_MODE; /* Since 1.0.6 */

extern GLogModule NFCDC_LOG_MODULE;

G_END_DECLS

#endif /* NFCDC_TYPES_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
