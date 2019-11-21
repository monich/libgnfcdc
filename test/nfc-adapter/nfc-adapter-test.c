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
    gulong id;

    if (app->adapter_path) {
        adapter = nfc_adapter_client_new(app->adapter_path);
        id = nfc_adapter_client_add_property_handler(adapter,
            NFC_ADAPTER_PROPERTY_ANY, app_adapter_changed, app);
    } else {
        da = nfc_default_adapter_new();
        id = nfc_default_adapter_add_property_handler(da,
            NFC_DEFAULT_ADAPTER_PROPERTY_ANY, app_default_adapter_changed,
            app);
    }

    app->ret = RET_ERR;
    app->loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(app->loop);
    g_source_remove(sigterm);
    g_source_remove(sigint);
    g_main_loop_unref(app->loop);
    app->loop = NULL;

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
