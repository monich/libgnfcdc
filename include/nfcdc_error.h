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

#ifndef NFCDC_ERROR_H
#define NFCDC_ERROR_H

#include <nfcdc_types.h>

G_BEGIN_DECLS

#define NFCDC_ERROR (nfcdc_error_quark())
GQuark nfcdc_error_quark(void);

#define NFCDC_ERRORS(e)                 \
    e(FAILED,           "Failed")       \
    e(ACCESS_DENIED,    "AccessDenied") \
    e(INVALID_ARGS,     "InvalidArgs")  \
    e(NOT_FOUND,        "NotFound")     \
    e(NOT_SUPPORTED,    "NotSupported") \
    e(ABORTED,          "Aborted")      \
    e(NACK,             "NACK")

typedef enum nfcdc_error_code {
    #define NFCDC_ERROR_ENUM_(E,e) NFCDC_ERROR_##E,
    NFCDC_ERRORS(NFCDC_ERROR_ENUM_)
    #undef NFCDC_ERROR_ENUM_
} NFCDC_ERROR_CODE;

gboolean
nfcdc_error_matches(
    const GError* error,
    NFCDC_ERROR_CODE code);

G_END_DECLS

#endif /* NFCDC_ERROR_H */

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
