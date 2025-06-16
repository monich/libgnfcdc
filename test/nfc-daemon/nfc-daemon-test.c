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

#include "nfcdc_daemon.h"

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
    NFC_MODE enable_modes;
    NFC_MODE disable_modes;
    NFC_TECH allow_techs;
    NFC_TECH disallow_techs;
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
app_property_changed(
    NfcDaemonClient* daemon,
    NFC_DAEMON_PROPERTY property,
    void* user_data)
{
    switch (property) {
    case NFC_DAEMON_PROPERTY_VALID:
        GDEBUG("Valid: %s", daemon->valid ? "true" : "false");
        break;
    case NFC_DAEMON_PROPERTY_PRESENT:
        GDEBUG("Present: %s", daemon->present ? "true" : "false");
        break;
    case NFC_DAEMON_PROPERTY_ERROR:
        GDEBUG("Error: %s", daemon->error ? daemon->error->message : "none");
        break;
    case NFC_DAEMON_PROPERTY_ENABLED:
        GDEBUG("Enabled: %s", daemon->enabled ? "true" : "false");
        break;
    case NFC_DAEMON_PROPERTY_ADAPTERS:
        GDEBUG("Adapters: %s", daemon->adapters[0] ? daemon->adapters[0] : "");
        break;
    case NFC_DAEMON_PROPERTY_VERSION:
        GDEBUG("Version: 0x%08x", daemon->version);
        break;
    case NFC_DAEMON_PROPERTY_MODE:
        GDEBUG("Mode: 0x%02x", daemon->mode);
        break;
    case NFC_DAEMON_PROPERTY_TECHS:
        GDEBUG("Techs: 0x%02x", daemon->techs);
        break;
    case NFC_DAEMON_PROPERTY_ANY:
    case NFC_DAEMON_PROPERTY_COUNT:
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
    NfcDaemonClient* daemon = nfc_daemon_client_new();
    gulong id = nfc_daemon_client_add_property_handler(daemon,
        NFC_DAEMON_PROPERTY_ANY, app_property_changed, app);
    NfcModeRequest* mode_req = NULL;
    NfcTechRequest* tech_req = NULL;

    if (app->enable_modes || app->disable_modes) {
        mode_req = nfc_mode_request_new(daemon, app->enable_modes,
            app->disable_modes);
    }
    if (app->allow_techs || app->disallow_techs) {
        tech_req = nfc_tech_request_new(daemon, app->allow_techs,
            app->disallow_techs);
    }
    app->ret = RET_ERR;
    app->loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(app->loop);

    g_source_remove(sigterm);
    g_source_remove(sigint);
    nfc_daemon_client_remove_handler(daemon, id);
    g_main_loop_unref(app->loop);
    app->loop = NULL;
    nfc_mode_request_free(mode_req);
    nfc_tech_request_free(tech_req);
    nfc_daemon_client_unref(daemon);
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
app_opt_enable_modes(
    const gchar* name,
    const gchar* value,
    gpointer user_data,
    GError** error)
{
    uint modes = 0;

    if (gutil_parse_uint(value, 0, &modes)) {
        App* app = user_data;

        app->enable_modes |= modes;
        return TRUE;
    } else {
        g_propagate_error(error, g_error_new(G_OPTION_ERROR,
            G_OPTION_ERROR_BAD_VALUE, "Invalid mode '%s'", value));
        return FALSE;
    }
}

static
gboolean
app_opt_disable_modes(
    const gchar* name,
    const gchar* value,
    gpointer user_data,
    GError** error)
{
    uint modes = 0;

    if (gutil_parse_uint(value, 0, &modes)) {
        App* app = user_data;

        app->disable_modes |= modes;
        return TRUE;
    } else {
        g_propagate_error(error, g_error_new(G_OPTION_ERROR,
            G_OPTION_ERROR_BAD_VALUE, "Invalid mode '%s'", value));
        return FALSE;
    }
}

static
gboolean
app_opt_allow_techs(
    const gchar* name,
    const gchar* value,
    gpointer user_data,
    GError** error)
{
    uint techs = 0;

    if (gutil_parse_uint(value, 0, &techs)) {
        App* app = user_data;

        app->allow_techs |= techs;
        return TRUE;
    } else {
        g_propagate_error(error, g_error_new(G_OPTION_ERROR,
            G_OPTION_ERROR_BAD_VALUE, "Invalid techs '%s'", value));
        return FALSE;
    }
}

static
gboolean
app_opt_disallow_techs(
    const gchar* name,
    const gchar* value,
    gpointer user_data,
    GError** error)
{
    uint techs = 0;

    if (gutil_parse_uint(value, 0, &techs)) {
        App* app = user_data;

        app->disallow_techs |= techs;
        return TRUE;
    } else {
        g_propagate_error(error, g_error_new(G_OPTION_ERROR,
            G_OPTION_ERROR_BAD_VALUE, "Invalid techs '%s'", value));
        return FALSE;
    }
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
        { "enable", 'm', G_OPTION_FLAG_NONE,
           G_OPTION_ARG_CALLBACK, &app_opt_enable_modes,
          "Enable modes (0x2:Read/Write, 0x3:P2P, 0x08:CE)", "mask" },
        { "disable", 'M', G_OPTION_FLAG_NONE,
           G_OPTION_ARG_CALLBACK, &app_opt_disable_modes,
          "Disable modes", "mask" },
        { "allow", 't', G_OPTION_FLAG_NONE,
           G_OPTION_ARG_CALLBACK, &app_opt_allow_techs,
          "Allow techs (0x1:A, 0x02:B, 0x4:F)", "mask" },
        { "disallow", 'T', G_OPTION_FLAG_NONE,
           G_OPTION_ARG_CALLBACK, &app_opt_disallow_techs,
          "Disallow techs", "mask" },
        { NULL }
    };
    GError* error = NULL;
    GOptionContext* options = g_option_context_new(NULL);
    GOptionGroup* group = g_option_group_new("", "", "", app, NULL);

    g_option_group_add_entries(group, entries);
    g_option_context_set_main_group(options, group);
    if (g_option_context_parse(options, &argc, &argv, &error)) {
        if (argc == 1) {
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
    gutil_log_set_type(GLOG_TYPE_STDERR, "nfc-daemon-test");
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
