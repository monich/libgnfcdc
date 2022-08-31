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

#include "nfcdc_log.h"

/* Log module */
GLOG_MODULE_DEFINE("nfc-client");

#if GUTIL_LOG_DEBUG

static inline
gboolean
nfcdc_blank_str(
    const char* prefix)
{
    return !prefix[strspn(prefix," \t")];
}

void
nfcdc_dump_strv(
    const char* prefix,
    const char* name,
    const char* sep,
    const GStrV* strv)
{
    if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
        if (strv) {
            GString* buf = g_string_new(NULL);
            const GStrV* ptr;

            for (ptr = strv; *ptr; ptr++) {
                if (buf->len > 0) {
                    g_string_append(buf, ", ");
                }
                g_string_append(buf, *ptr);
            }
            if (prefix) {
                if (sep) {
                    GDEBUG("%s%s%s %s {%s}", prefix, nfcdc_blank_str(prefix) ?
                               "" : ": ", name, sep, buf->str);
                } else {
                    GDEBUG("%s: %s {%s}", prefix, name, buf->str);
                }
            } else if (sep) {
                GDEBUG("%s %s {%s}", name, sep, buf->str);
            } else {
                GDEBUG("%s {%s}", name, buf->str);
            }
            g_string_free(buf, TRUE);
        } else {
            if (prefix) {
                GDEBUG("%s%s%s %s", prefix, nfcdc_blank_str(prefix) ?
                    "" : ": ", name, sep);
            } else if (sep) {
                GDEBUG("%s %s", name, sep);
            } else {
                GDEBUG("%s", name);
            }
        }
    }
}

void
nfcdc_dump_data(
    const char* prefix,
    const char* name,
    const char* sep,
    const GUtilData* data)
{
    if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
        if (data && data->size) {
            GString* buf = g_string_new(NULL);
            gsize i;

            g_string_printf(buf, "%02X", data->bytes[0]);
            for (i = 1; i < data->size; i++) {
                g_string_append_printf(buf, ":%02X", data->bytes[i]);
            }
            if (prefix) {
                GDEBUG("%s%s%s %s %s", prefix, nfcdc_blank_str(prefix) ?
                    "" : ": ", name, sep, buf->str);
            } else {
                GDEBUG("%s %s %s", name, sep, buf->str);
            }
            g_string_free(buf, TRUE);
        } else if (prefix) {
            GDEBUG("%s%s%s %s", prefix, nfcdc_blank_str(prefix) ?
                "" : ": ", name, sep);
        } else if (sep) {
            GDEBUG("%s %s", name, sep);
        } else {
            GDEBUG("%s", name);
        }
    }
}

void
nfcdc_dump_bytes(
    const char* prefix,
    const char* name,
    const char* sep,
    GBytes* bytes)
{
    if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
        if (bytes) {
            GUtilData data;

            data.bytes = g_bytes_get_data(bytes, &data.size);
            nfcdc_dump_data(prefix, name, sep, &data);
        } else {
            nfcdc_dump_data(prefix, name, sep, NULL);
        }
    }
}

#endif /* GUTIL_LOG_DEBUG */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
