/*
 * Copyright (C) 2022 Jolla Ltd.
 * Copyright (C) 2022 Slava Monich <slava@monich.com>
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

#include "nfcdc_util_p.h"
#include "nfcdc_log.h"

#include <gutil_misc.h>

GUtilData*
nfc_data_copy(
    const void* data,
    gsize size)
{
    /* Allocates GUtilData and the actual data from a single memory block */
    if (data) {
        GUtilData* copy = g_malloc(sizeof(GUtilData) + size);
        void* buf = copy + 1;

        copy->size = size;
        copy->bytes = buf;
        memcpy(buf, data, size);
        return copy;
    } else {
        return g_new0(GUtilData, 1);
    }
}

GUtilData*
nfc_data_from_variant(
    GVariant* var)
{
    GUtilData* data = NULL;
    GVariant* tmp = NULL;

    if (g_variant_is_of_type(var, G_VARIANT_TYPE_VARIANT)) {
        var = tmp = g_variant_get_variant(var);
    }

    /* Allocate value as a single memory block */
    if (g_variant_is_of_type(var, G_VARIANT_TYPE_BYTESTRING)) {
        data = nfc_data_copy(g_variant_get_data(var), g_variant_get_size(var));
    } else if (g_variant_is_of_type(var, G_VARIANT_TYPE_BYTE)) {
        const guint8 b = g_variant_get_byte(var);
        data = nfc_data_copy(&b, 1);
    }

    if (tmp) {
        g_variant_unref(tmp);
    }

    /* Dispose of GUtilData and its contents with a single g_free() */
    return data;
}

GHashTable*
nfc_parse_dict(
    GHashTable* params,
    GVariant* dict,
    NfcStringKeyFunc string_key)
{
    if (dict) {
        GVariantIter it;
        GVariant* entry;

        if (!params) {
            params = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                NULL, g_free);
        }

        for (g_variant_iter_init(&it, dict);
             (entry = g_variant_iter_next_value(&it)) != NULL;
             g_variant_unref(entry)) {
            GVariant* dict_key = g_variant_get_child_value(entry, 0);
            const char* name = g_variant_get_string(dict_key, NULL);
            const int key = string_key(name);

            if (key >= 0) {
                GVariant* dict_value = g_variant_get_child_value(entry, 1);
                GUtilData* data = nfc_data_from_variant(dict_value);

                if (data) {
                    DUMP_DATA("  ", name, "=", data);
                    g_hash_table_insert(params, GINT_TO_POINTER(key), data);
                }
                g_variant_unref(dict_value);
            }
            g_variant_unref(dict_key);
        }
    }
    return params;
}

gboolean
nfc_params_equal(
    GHashTable* params1,
    GHashTable* params2)
{
    if (params1 == params2) {
        return TRUE;
    } else if (!params1 || !params2) {
        return FALSE;
    } else {
        const guint size1 = g_hash_table_size(params1);
        const guint size2 = g_hash_table_size(params2);

        if (size1 != size2) {
            return FALSE;
        } else if (size1) {
            GHashTableIter it;
            gpointer key, value;

            g_hash_table_iter_init(&it, params1);
            while (g_hash_table_iter_next(&it, &key, &value)) {
                if (!gutil_data_equal(g_hash_table_lookup(params2,
                    key), value)) {
                    return FALSE;
                }
            }
        }
        return TRUE;
    }
}

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
