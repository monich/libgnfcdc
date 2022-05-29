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

#ifndef NFCDC_LOG_H
#define NFCDC_LOG_H

#include "nfcdc_types.h"

#define GLOG_MODULE_NAME NFCDC_LOG_MODULE

#include <gutil_log.h>

#if GUTIL_LOG_DEBUG

void
nfcdc_dump_strv(
    const char* prefix,
    const char* name,
    const char* sep,
    const GStrV* strv)
    G_GNUC_INTERNAL;

void
nfcdc_dump_data(
    const char* prefix,
    const char* name,
    const char* sep,
    const GUtilData* data)
    G_GNUC_INTERNAL;

void
nfcdc_dump_bytes(
    const char* prefix,
    const char* name,
    const char* sep,
    GBytes* bytes)
    G_GNUC_INTERNAL;

#  define DUMP_STRV(prefix,name,sep,strv) \
   nfcdc_dump_strv(prefix,name,sep,strv)
#  define DUMP_DATA(prefix,name,sep,data) \
   nfcdc_dump_data(prefix,name,sep,data)
#  define DUMP_BYTES(prefix,name,sep,bytes) \
   nfcdc_dump_bytes(prefix,name,sep,bytes)
#else
#  define DUMP_STRV(prefix,name,sep,strv) ((void)0)
#  define DUMP_DATA(prefix,name,sep,data) ((void)0)
#  define DUMP_BYTES(prefix,name,sep,bytes) ((void)0)
#endif

#endif /* NFCDC_LOG_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
