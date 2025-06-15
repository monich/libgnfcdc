/*
 * Copyright (C) 2019-2025 Slava Monich <slava@monich.com>
 * Copyright (C) 2019 Jolla Ltd.
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

#include "nfcdc_default_adapter.h"

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
    const char* adapter_path;
    GHashTable* params;
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

void
app_dump_data(
    const char* name,
    const GUtilData* data)
{
    if (GLOG_ENABLED(GLOG_LEVEL_DEBUG)) {
        if (data) {
            if (data->size) {
                GString* buf = g_string_new(NULL);
                gsize i;

                g_string_printf(buf, "%02X", data->bytes[0]);
                for (i = 1; i < data->size; i++) {
                    g_string_append_printf(buf, ":%02X", data->bytes[i]);
                }
                GDEBUG("%s: %s", name, buf->str);
                g_string_free(buf, TRUE);
            } else {
                GDEBUG("%s:", name);
            }
        } else {
            GDEBUG("%s: (null)", name);
        }
    }
}

static
void
app_adapter_changed(
    NfcAdapterClient* adapter,
    NFC_ADAPTER_PROPERTY property,
    void* user_data)
{
    switch (property) {
    case NFC_ADAPTER_PROPERTY_VALID:
        GDEBUG("Valid: %s", adapter->valid ? "true" : "false");
        break;
    case NFC_ADAPTER_PROPERTY_PRESENT:
        GDEBUG("Present: %s", adapter->present ? "true" : "false");
        break;
    case NFC_ADAPTER_PROPERTY_ENABLED:
        GDEBUG("Enabled: %s", adapter->enabled ? "true" : "false");
        break;
    case NFC_ADAPTER_PROPERTY_POWERED:
        GDEBUG("Powered: %s", adapter->powered ? "true" : "false");
        break;
    case NFC_ADAPTER_PROPERTY_MODE:
        GDEBUG("Mode: 0x%02X", adapter->mode);
        break;
    case NFC_ADAPTER_PROPERTY_TARGET_PRESENT:
        GDEBUG("Target: %sresent", adapter->target_present ? "P" : "Not p");
        break;
    case NFC_ADAPTER_PROPERTY_TAGS:
        GDEBUG("Tags: %s", adapter->tags[0] ? adapter->tags[0] : "");
        break;
    case NFC_ADAPTER_PROPERTY_PEERS:
        GDEBUG("Peers: %s", adapter->peers[0] ? adapter->peers[0] : "");
        break;
    case NFC_ADAPTER_PROPERTY_HOSTS:
        GDEBUG("Hosts: %s", adapter->hosts[0] ? adapter->hosts[0] : "");
        break;
    case NFC_ADAPTER_PROPERTY_T4_NDEF:
        GDEBUG("T4_NDEF: %s", adapter->t4_ndef ? "on" : "off");
        break;
    case NFC_ADAPTER_PROPERTY_LA_NFCID1:
        app_dump_data("LA_NFCID1", adapter->la_nfcid1);
        break;
    case NFC_ADAPTER_PROPERTY_ANY:
    case NFC_ADAPTER_PROPERTY_COUNT:
        break;
    }
}

static
void
app_default_adapter_changed(
    NfcDefaultAdapter* da,
    NFC_DEFAULT_ADAPTER_PROPERTY property,
    void* user_data)
{
    switch (property) {
    case NFC_DEFAULT_ADAPTER_PROPERTY_VALID:
        GDEBUG("Valid: %s", da->valid ? "true" : "false");
        break;
    case NFC_DEFAULT_ADAPTER_PROPERTY_ADAPTER:
        GDEBUG("Adapter: %s", da->adapter ? da->adapter->path : "none");
        break;
    case NFC_DEFAULT_ADAPTER_PROPERTY_ENABLED:
        GDEBUG("Enabled: %s", da->enabled ? "true" : "false");
        break;
    case NFC_DEFAULT_ADAPTER_PROPERTY_POWERED:
        GDEBUG("Powered: %s", da->powered ? "true" : "false");
        break;
    case NFC_DEFAULT_ADAPTER_PROPERTY_SUPPORTED_MODES:
        GDEBUG("Supported modes: 0x%02X", da->supported_modes);
        break;
    case NFC_DEFAULT_ADAPTER_PROPERTY_MODE:
        GDEBUG("Mode: 0x%02X", da->mode);
        break;
    case NFC_DEFAULT_ADAPTER_PROPERTY_TARGET_PRESENT:
        GDEBUG("Target: %sresent", da->target_present ? "P" : "Not p");
        break;
    case NFC_DEFAULT_ADAPTER_PROPERTY_TAGS:
        GDEBUG("Tags: %s", da->tags[0] ? da->tags[0] : "");
        break;
    case NFC_DEFAULT_ADAPTER_PROPERTY_PEERS:
        GDEBUG("Peers: %s", da->peers[0] ? da->peers[0] : "");
        break;
    case NFC_DEFAULT_ADAPTER_PROPERTY_HOSTS:
        GDEBUG("Hosts: %s", da->hosts[0] ? da->hosts[0] : "");
        break;
    case NFC_DEFAULT_ADAPTER_PROPERTY_SUPPORTED_TECHS:
        GDEBUG("Supported techs: 0x%02X", da->supported_techs);
        break;
    case NFC_DEFAULT_ADAPTER_PROPERTY_T4_NDEF:
        GDEBUG("T4_NDEF: %s", da->t4_ndef ? "on" : "off");
        break;
    case NFC_DEFAULT_ADAPTER_PROPERTY_LA_NFCID1:
        app_dump_data("LA_NFCID1", da->la_nfcid1);
        break;
    case NFC_DEFAULT_ADAPTER_PROPERTY_ANY:
    case NFC_DEFAULT_ADAPTER_PROPERTY_COUNT:
        break;
    }
}

static
int
app_run(
    App* app)
{
    guint sigterm = g_unix_signal_add(SIGTERM, app_signal, app);
    guint sigint = g_unix_signal_add(SIGINT, app_signal, app);
    NfcAdapterClient* adapter = NULL;
    NfcDefaultAdapter* da = NULL;
    NfcAdapterParamReq* apr = NULL;
    NfcDefaultAdapterParamReq* dapr = NULL;
    GPtrArray* params = NULL;
    gulong id;

    if (app->params) {
        GHashTableIter it;
        gpointer value;

        params = g_ptr_array_sized_new(g_hash_table_size(app->params) + 1);
        g_hash_table_iter_init(&it, app->params);
        while (g_hash_table_iter_next(&it, NULL, &value)) {
            g_ptr_array_add(params, value);
        }
        g_ptr_array_add(params, NULL); /* NULL terminate */
    }

    if (app->adapter_path) {
        adapter = nfc_adapter_client_new(app->adapter_path);
        id = nfc_adapter_client_add_property_handler(adapter,
            NFC_ADAPTER_PROPERTY_ANY, app_adapter_changed, app);
        if (params) {
            apr = nfc_adapter_param_req_new(adapter, FALSE,
                (const NfcAdapterParamPtrC*) params->pdata);
        }
    } else {
        da = nfc_default_adapter_new();
        id = nfc_default_adapter_add_property_handler(da,
            NFC_DEFAULT_ADAPTER_PROPERTY_ANY, app_default_adapter_changed,
            app);
        if (params) {
            dapr = nfc_default_adapter_param_req_new(da, FALSE,
                (const NfcAdapterParamPtrC*) params->pdata);
        }
    }

    app->ret = RET_ERR;
    app->loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(app->loop);
    g_source_remove(sigterm);
    g_source_remove(sigint);
    nfc_adapter_param_req_free(apr);
    nfc_default_adapter_param_req_free(dapr);
    g_main_loop_unref(app->loop);
    app->loop = NULL;
    if (app->params) {
        g_ptr_array_free(params, TRUE);
        g_hash_table_destroy(app->params);
        app->params = NULL;
    }

    if (adapter) {
        nfc_adapter_client_remove_handler(adapter, id);
        nfc_adapter_client_unref(adapter);
    } else {
        nfc_default_adapter_remove_handler(da, id);
        nfc_default_adapter_unref(da);
    }
    return app->ret;
}

static
void
app_add_param(
    App* app,
    NfcAdapterParam* param)
{
    if (!app->params) {
        app->params = g_hash_table_new_full(g_direct_hash, g_direct_equal,
            NULL, g_free);
    }

    /* Take ownership of the param */
    g_hash_table_insert(app->params, GINT_TO_POINTER(param->key), param);
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
app_opt_t4_ndef(
    const char* name,
    const char* value,
    gpointer user_data,
    GError** error)
{
    App* app = user_data;
    NfcAdapterParam param;

    memset(&param, 0, sizeof(param));
    param.key = NFC_ADAPTER_PARAM_KEY_T4_NDEF;
    param.value.b = TRUE;

    if (value) {
        if (!g_ascii_strcasecmp(value, "off") ||
            !g_ascii_strcasecmp(value, "false")) {
            param.value.b = FALSE;
        } else if (g_ascii_strcasecmp(value, "on") &&
            g_ascii_strcasecmp(value, "true")) {
            g_propagate_error(error, g_error_new(G_OPTION_ERROR,
                G_OPTION_ERROR_BAD_VALUE, "Invalid t4_ndef value '%s'",
                value));
            return FALSE;
        }
    }

    app_add_param(app, gutil_memdup(&param, sizeof(param)));
    return TRUE;
}

static
gboolean
app_opt_nfcid1(
    const char* name,
    const char* value,
    gpointer user_data,
    GError** error)
{
    App* app = user_data;
    GBytes* bytes = NULL;
    NfcAdapterParam* param;
    GUtilData data;

    if (value) {
        if ((bytes = gutil_hex2bytes(value, -1))) {
            data.bytes = g_bytes_get_data(bytes, &data.size);
        } else {
            g_propagate_error(error, g_error_new(G_OPTION_ERROR,
                G_OPTION_ERROR_BAD_VALUE, "Invalid hex data '%s'", value));
            return FALSE;
        }
    } else {
        memset(&data, 0, sizeof(data));
    }

    param = g_malloc0(sizeof(NfcAdapterParam) + data.size);
    param->key = NFC_ADAPTER_PARAM_KEY_LA_NFCID1;
    if ((param->value.data.size = data.size)) {
        void* pdata = param + 1;

        param->value.data.bytes = pdata;
        memcpy(pdata, data.bytes, data.size);
    }

    if (bytes) {
        g_bytes_unref(bytes);
    }
    app_add_param(app, param);
    return TRUE;
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
        { "t4-ndef", 0, G_OPTION_FLAG_OPTIONAL_ARG,
           G_OPTION_ARG_CALLBACK, &app_opt_t4_ndef,
          "Set T4_NDEF option (request NDEF from Type4 tags)", "[on|off]" },
        { "nfcid1", 0, G_OPTION_FLAG_OPTIONAL_ARG,
           G_OPTION_ARG_CALLBACK, &app_opt_nfcid1,
          "Set LA_NFCID1 option (NFCID1 in NFC-A Listen mode)", "hex" },
        { NULL }
    };
    GError* error = NULL;
    GOptionContext* options = g_option_context_new("[PATH]");
    GOptionGroup* group = g_option_group_new("", "", "", app, NULL);

    g_option_group_add_entries(group, entries);
    g_option_context_set_main_group(options, group);
    if (g_option_context_parse(options, &argc, &argv, &error)) {
        if (argc < 3) {
            if (argc == 2) {
                app->adapter_path = argv[1];
            }
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
    gutil_log_set_type(GLOG_TYPE_STDERR, "nfc-adapter-test");
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
