/*
 * Copyright (C) 2019 Jolla Ltd.
 * Copyright (C) 2019 Slava Monich <slava@monich.com>
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

#include "nfcdc_default_adapter.h"
#include "nfcdc_isodep.h"

#include <gutil_log.h>
#include <gutil_misc.h>
#include <gutil_macros.h>

#include <glib-unix.h>

#define RET_OK (0)
#define RET_ERR (1)
#define RET_CANCEL (2)

typedef struct app {
    GMainLoop* loop;
    gboolean stopped;
    NfcIsoDepApdu apdu;
    NfcIsoDepClient* tag;
    gulong tag_event_id;
    void* bytes;
    int ret;
} App;

static
gboolean
app_signal(
    gpointer user_data)
{
    App* app = user_data;

    if (!app->stopped) {
        app->stopped = TRUE;
        app->ret = RET_CANCEL;
        GDEBUG("Signal caught, exiting...");
        g_main_loop_quit(app->loop);
    }
    return G_SOURCE_CONTINUE;
}

static
void
app_debug_hexdump(
    const GUtilData* data)
{
    if (data) {
        const guint8* ptr = data->bytes;
        gsize len = data->size;
        guint off = 0;

        while (len > 0) {
            char buf[GUTIL_HEXDUMP_BUFSIZE];
            const guint consumed = gutil_hexdump(buf, ptr + off, len);

            GDEBUG("  %04X: %s", off, buf);
            len -= consumed;
            off += consumed;
        }
    }
}

static
void
app_transmit_done(
    NfcIsoDepClient* isodep,
    const GUtilData* response,
    guint sw,  /* 16 bits (SW1 << 8)|SW2 */
    const GError* error,
    void* user_data)
{
    if (error) {
        GERR("%s", GERRMSG(error));
    } else {
        GINFO("Response %04X, %u byte(s)", sw, response->size);
        app_debug_hexdump(response);
    }
}

static
void
app_transmit(
    App* app)
{
    GDEBUG("Sending APDU");
    GVERIFY(nfc_isodep_client_transmit(app->tag, &app->apdu, NULL,
        app_transmit_done, app, NULL));
}

static
void
app_isodep_changed(
    NfcIsoDepClient* isodep,
    NFC_ISODEP_PROPERTY property,
    void* user_data)
{
    switch (property) {
    case NFC_ISODEP_PROPERTY_VALID:
        GDEBUG("Valid: %s", isodep->valid ? "true" : "false");
        if (isodep->present) {
            app_transmit((App*)user_data);
        } else {
            GINFO("Not an ISO-DEP tag");
        }
        break;
    case NFC_ISODEP_PROPERTY_PRESENT:
        GDEBUG("Present: %s", isodep->present ? "true" : "false");
        break;
    case NFC_ISODEP_PROPERTY_ANY:
    case NFC_ISODEP_PROPERTY_COUNT:
        break;
    }
}

static
void
app_update_tag(
    App* app,
    const GStrV* tags)
{
    if (tags && tags[0]) {
        const char* path = tags[0];

        if (!app->tag || strcmp(app->tag->path, path)) {
            GDEBUG("Tag %s found", tags[0]);
            nfc_isodep_client_remove_handlers(app->tag, &app->tag_event_id, 1);
            nfc_isodep_client_unref(app->tag);
            app->tag = nfc_isodep_client_new(path);
            app->tag_event_id = nfc_isodep_client_add_property_handler(app->tag,
                NFC_ISODEP_PROPERTY_ANY, app_isodep_changed, app);
            if (app->tag->present) {
                app_transmit(app);
            }
        }       
    } else if (app->tag) {
        GDEBUG("Tag %s is gone", app->tag->path);
        nfc_isodep_client_remove_handlers(app->tag, &app->tag_event_id, 1);
        nfc_isodep_client_unref(app->tag);
        app->tag = NULL;
    }
}

static
void
app_tags_changed(
    NfcDefaultAdapter* da,
    NFC_DEFAULT_ADAPTER_PROPERTY property,
    void* user_data)
{
    app_update_tag((App*)user_data, da->tags);
}

static
int
app_run(
    App* app)
{
    guint sigterm = g_unix_signal_add(SIGTERM, app_signal, app);
    guint sigint = g_unix_signal_add(SIGINT, app_signal, app);
    NfcDefaultAdapter* da = nfc_default_adapter_new();
    gulong da_id = nfc_default_adapter_add_property_handler(da,
        NFC_DEFAULT_ADAPTER_PROPERTY_TAGS, app_tags_changed, app);

    app_update_tag(app, da->tags);
    app->ret = RET_ERR;
    app->loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(app->loop);
    g_source_remove(sigterm);
    g_source_remove(sigint);
    g_main_loop_unref(app->loop);
    app->loop = NULL;
    app_update_tag(app, NULL);
    if (da) {
        nfc_default_adapter_remove_handler(da, da_id);
        nfc_default_adapter_unref(da);
    }
    g_free(app->bytes);
    return app->ret;
}

static
gboolean
app_opt_verbose(
    const gchar* name,
    const gchar* value,
    gpointer user_data,
    GError** error)
{
    gutil_log_default.level = GLOG_LEVEL_VERBOSE;
    return TRUE;
}

static
gboolean
app_opt_quiet(
    const gchar* name,
    const gchar* value,
    gpointer user_data,
    GError** error)
{
    gutil_log_default.level = GLOG_LEVEL_ERR;
    return TRUE;
}

static
gboolean
app_parse_hex_word(
    const char* str,
    guint* out)
{
    /* Length 1 or 2, 3 or 4 is expected */
    if (str[0]) {
        const gsize len = strlen(str);

        if (len < 5) {
            char buf[5];
            guint16 result;

            /* Pad with zeros if necessary */
            if (len < 4) {
                guint i = 0;

                switch (len) {
                case 3: buf[i++] = '0';
                case 2: buf[i++] = '0';
                case 1: buf[i++] = '0';
                default: buf[i] = 0;
                }
                strcpy(buf + i, str);
                str = buf;
            }

            if (gutil_hex2bin(str, 4, &result)) {
                *out = GINT16_FROM_BE(result);
                return TRUE;
            }
        }
    }
    return FALSE;
}

static
gboolean
app_parse_hex_byte(
    const char* str,
    guint8* out)
{
    /* Length 1 or 2 is expected */
    if (str[0] && (!str[1] || !str[2])) {
        char buf[3];
        guint8 result;

        /* Pad with zero if necessary */
        if (!str[1]) {
            buf[0] = '0';
            buf[1] = str[0];
            buf[2] = 0;
            str = buf;
        }

        if (gutil_hex2bin(str, 2, &result)) {
            *out = result;
            return TRUE;
        }
    }
    return FALSE;
}

static
gboolean
app_parse_args(
    App* app,
    int argc,
    char** argv)
{
    NfcIsoDepApdu* apdu = &app->apdu;

    /* CLA ISO P1 P2 [DATA [LE]] */
    if (argc >= 5 && argc <= 7 &&
        app_parse_hex_byte(argv[1], &apdu->cla) &&
        app_parse_hex_byte(argv[2], &apdu->ins) &&
        app_parse_hex_byte(argv[3], &apdu->p1) &&
        app_parse_hex_byte(argv[4], &apdu->p2)) {
        if (argc == 5) {
            return TRUE;
        } else {
            const char* data = argv[5];
            const gsize len = strlen(data);

            if (!(len & 1)) {
                apdu->data.size = len/2;
                app->bytes = g_malloc(apdu->data.size);
                if (!len || gutil_hex2bin(data, len, app->bytes)) {
                    apdu->data.bytes = app->bytes;
                    if (argc == 6) {
                        return TRUE;
                    } else {
                        const char* le = argv[6];

                        if (!strcmp(le, "00")) {
                            apdu->le = 0x100;
                            return TRUE;
                        } else if (!strcmp(le, "0000")) {
                            apdu->le = 0x10000;
                            return TRUE;
                        } else if (app_parse_hex_word(argv[6], &apdu->le)) {
                            return TRUE;
                        }
                    }
                }
                g_free(app->bytes);
                app->bytes = NULL;
            }
        }
    }
    return FALSE;
}

static
gboolean
app_init(
    App* app,
    int argc,
    char* argv[])
{
    gboolean ok = FALSE;
    GOptionEntry entries[] = {
        { "verbose", 'v', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK,
          app_opt_verbose, "Enable verbose output", NULL },
        { "quiet", 'q', G_OPTION_FLAG_NO_ARG, G_OPTION_ARG_CALLBACK,
          app_opt_quiet, "Be quiet", NULL },
        { NULL }
    };
    GError* error = NULL;
    GOptionContext* options = g_option_context_new("[PATH]");

    g_option_context_add_main_entries(options, entries, NULL);
    if (g_option_context_parse(options, &argc, &argv, &error)) {
        if (app_parse_args(app, argc, argv)) {
            ok = TRUE;
        } else {
            char* help = g_option_context_get_help(options, TRUE, NULL);

            fprintf(stderr, "%s", help);
            g_free(help);
        }
    } else {
        GERR("%s", error->message);
        g_error_free(error);
    }
    g_option_context_free(options);
    return ok;
}
int main(int argc, char* argv[])
{
    int ret = RET_ERR;
    App app;

    memset(&app, 0, sizeof(app));
    gutil_log_set_type(GLOG_TYPE_STDERR, "nfc-tag-test");
    if (app_init(&app, argc, argv)) {
        ret = app_run(&app);
    }
    return ret;
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
