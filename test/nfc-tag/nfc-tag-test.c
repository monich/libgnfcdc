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
#include "nfcdc_tag.h"

#include <gutil_log.h>
#include <gutil_misc.h>
#include <gutil_macros.h>

#include <glib-unix.h>

#define RET_OK (0)
#define RET_ERR (1)
#define RET_CANCEL (2)

#define LOCK_PERIOD_SEC (2)

typedef struct app {
    GMainLoop* loop;
    gboolean stopped;
    gboolean do_lock;
    NfcTagClient* tag;
    NfcTagClientLock* lock;
    GCancellable* lock_cancel;
    gulong tag_event_id;
    const char* tag_path;
    guint lock_timeout;
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
app_dump_strv(
    const char* prefix,
    const GStrV* sv)
{
    GString* buf = g_string_new(NULL);

    while (*sv) {
        if (buf->len) g_string_append(buf, ", ");
        g_string_append(buf, *sv++);
    }
    GDEBUG("%s: %s", prefix, buf->str);
    g_string_free(buf, TRUE);
}

static
gboolean
app_tag_lock_expired(
    gpointer user_data)
{
    App* app = user_data;

    GDEBUG("Lock expired");
    nfc_tag_client_lock_unref(app->lock);
    app->lock_timeout = 0;
    app->lock = NULL;
    return G_SOURCE_REMOVE;
}

static
void
app_tag_locked(
    NfcTagClient* tag,
    NfcTagClientLock* lock,
    const GError* error,
    void* user_data)
{
    App* app = user_data;

    if (lock) {
        app->lock = nfc_tag_client_lock_ref(lock);
        app->lock_timeout = g_timeout_add_seconds(LOCK_PERIOD_SEC,
            app_tag_lock_expired, app);
    } else {
        GERR("Failed to lock the tag: %s", GERRMSG(error));
    }
    g_object_unref(app->lock_cancel);
    app->lock_cancel = NULL;
}

static
void
app_drop_lock(
    App* app)
{
    if (app->lock_cancel) {
        g_cancellable_cancel(app->lock_cancel);
        g_object_unref(app->lock_cancel);
        app->lock_cancel = NULL;
    }
    if (app->lock) {
        nfc_tag_client_lock_unref(app->lock);
        app->lock = NULL;
    }
    if (app->lock_timeout) {
        g_source_remove(app->lock_timeout);
        app->lock_timeout = 0;
    }
}

static
void
app_lock_tag(
    App* app)
{
    GDEBUG("Locking the tag");
    GASSERT(app->tag);
    GASSERT(!app->lock);
    GASSERT(!app->lock_cancel);
    app->lock_cancel = g_cancellable_new();
    GVERIFY(nfc_tag_client_acquire_lock(app->tag, TRUE, app->lock_cancel,
        app_tag_locked, app, NULL));
}

static
void
app_tag_changed(
    NfcTagClient* tag,
    NFC_TAG_PROPERTY property,
    void* user_data)
{
    App* app = user_data;

    switch (property) {
    case NFC_TAG_PROPERTY_VALID:
        GDEBUG("Valid: %s", tag->valid ? "true" : "false");
        break;
    case NFC_TAG_PROPERTY_PRESENT:
        GDEBUG("Present: %s", tag->present ? "true" : "false");
        if (tag->present && app->do_lock && !app->lock_cancel && !app->lock) {
            app_lock_tag(app);
        }
        break;
    case NFC_TAG_PROPERTY_INTERFACES:
        app_dump_strv("Interfaces", tag->interfaces);
        break;
    case NFC_TAG_PROPERTY_NDEF_RECORDS:
        app_dump_strv("NDEF records", tag->ndef_records);
        break;
    case NFC_TAG_PROPERTY_ANY:
    case NFC_TAG_PROPERTY_COUNT:
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
            app_drop_lock(app);
            nfc_tag_client_remove_handlers(app->tag, &app->tag_event_id, 1);
            nfc_tag_client_unref(app->tag);
            app->tag = nfc_tag_client_new(path);
            app->tag_event_id = nfc_tag_client_add_property_handler(app->tag,
                NFC_TAG_PROPERTY_ANY, app_tag_changed, app);
            if (app->do_lock && app->tag->present) {
                app_lock_tag(app);
            }
        }       
    } else if (app->tag) {
        GDEBUG("Tag %s is gone", app->tag->path);
        app_drop_lock(app);
        nfc_tag_client_remove_handlers(app->tag, &app->tag_event_id, 1);
        nfc_tag_client_unref(app->tag);
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
    NfcDefaultAdapter* da = NULL;
    gulong da_id = 0;

    if (app->tag_path) {
        app->tag = nfc_tag_client_new(app->tag_path);
        app->tag_event_id = nfc_tag_client_add_property_handler(app->tag,
            NFC_TAG_PROPERTY_ANY, app_tag_changed, app);
    } else {
        da = nfc_default_adapter_new();
        app_update_tag(app, da->tags);
        da_id = nfc_default_adapter_add_property_handler(da,
            NFC_DEFAULT_ADAPTER_PROPERTY_TAGS, app_tags_changed, app);
    }

    app->ret = RET_ERR;
    app->loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(app->loop);
    g_source_remove(sigterm);
    g_source_remove(sigint);
    g_main_loop_unref(app->loop);
    app->loop = NULL;
    app_drop_lock(app);
    app_update_tag(app, NULL);
    if (da) {
        nfc_default_adapter_remove_handler(da, da_id);
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
        { "lock", 'l', 0, G_OPTION_ARG_NONE, &app->do_lock,
          "Lock the tag", NULL },
        { NULL }
    };
    GError* error = NULL;
    GOptionContext* options = g_option_context_new("[PATH]");

    g_option_context_add_main_entries(options, entries, NULL);
    if (g_option_context_parse(options, &argc, &argv, &error)) {
        if (argc < 3) {
            if (argc == 2) {
                app->tag_path = argv[1];
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
