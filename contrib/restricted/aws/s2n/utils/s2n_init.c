/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */
#include <pthread.h>

#include "crypto/s2n_fips.h"
#include "crypto/s2n_libcrypto.h"
#include "crypto/s2n_locking.h"
#include "error/s2n_errno.h"
#include "openssl/opensslv.h"
#include "pq-crypto/s2n_pq.h"
#include "tls/extensions/s2n_client_key_share.h"
#include "tls/extensions/s2n_extension_type.h"
#include "tls/s2n_cipher_suites.h"
#include "tls/s2n_security_policies.h"
#include "tls/s2n_tls13_secrets.h"
#include "utils/s2n_mem.h"
#include "utils/s2n_random.h"
#include "utils/s2n_safety.h"
#include "utils/s2n_safety_macros.h"

static void s2n_cleanup_atexit(void);

static pthread_t main_thread = 0;
static bool initialized = false;
static bool atexit_cleanup = true;
int s2n_disable_atexit(void)
{
    POSIX_ENSURE(!initialized, S2N_ERR_INITIALIZED);
    atexit_cleanup = false;
    return S2N_SUCCESS;
}

int s2n_enable_atexit(void)
{
    atexit_cleanup = true;
    return S2N_SUCCESS;
}

int s2n_init(void)
{
    /* USAGE-GUIDE says s2n_init MUST NOT be called more than once
     * Public documentation for API states s2n_init should only be called once
     * https://github.com/aws/s2n-tls/issues/3446 is a result of not enforcing this
     */
    POSIX_ENSURE(!initialized, S2N_ERR_INITIALIZED);

    main_thread = pthread_self();
    /* Should run before any init method that calls libcrypto methods
     * to ensure we don't try to call methods that don't exist.
     * It doesn't require any locks since it only deals with values that
     * should be constant, so can run before s2n_locking_init. */
    POSIX_GUARD_RESULT(s2n_libcrypto_validate_runtime());
    /* Must run before any init method that allocates memory. */
    POSIX_GUARD(s2n_mem_init());
    /* Must run before any init method that calls libcrypto methods. */
    POSIX_GUARD_RESULT(s2n_locking_init());
    POSIX_GUARD(s2n_fips_init());
    POSIX_GUARD_RESULT(s2n_rand_init());
    POSIX_GUARD(s2n_cipher_suites_init());
    POSIX_GUARD(s2n_security_policies_init());
    POSIX_GUARD(s2n_config_defaults_init());
    POSIX_GUARD(s2n_extension_type_init());
    POSIX_GUARD_RESULT(s2n_pq_init());
    POSIX_GUARD_RESULT(s2n_tls13_empty_transcripts_init());

    if (atexit_cleanup) {
        POSIX_ENSURE_OK(atexit(s2n_cleanup_atexit), S2N_ERR_ATEXIT);
    }

    if (getenv("S2N_PRINT_STACKTRACE")) {
        s2n_stack_traces_enabled_set(true);
    }

    initialized = true;

    return S2N_SUCCESS;
}

static bool s2n_cleanup_atexit_impl(void)
{
    /* all of these should run, regardless of result, but the
     * values to need to be consumed to prevent warnings */

    /* the configs need to be wiped before resetting the memory callbacks */
    s2n_wipe_static_configs();

    bool cleaned_up = s2n_result_is_ok(s2n_cipher_suites_cleanup())
            && s2n_result_is_ok(s2n_rand_cleanup_thread())
            && s2n_result_is_ok(s2n_rand_cleanup())
            && s2n_result_is_ok(s2n_locking_cleanup())
            && (s2n_mem_cleanup() == S2N_SUCCESS);

    initialized = !cleaned_up;
    return cleaned_up;
}

int s2n_cleanup(void)
{
    /* s2n_cleanup is supposed to be called from each thread before exiting,
     * so ensure that whatever clean ups we have here are thread safe */
    POSIX_GUARD_RESULT(s2n_rand_cleanup_thread());

    /* If this is the main thread and atexit cleanup is disabled,
     * perform final cleanup now */
    if (pthread_equal(pthread_self(), main_thread) && !atexit_cleanup) {
        /* some cleanups are not idempotent (rand_cleanup, mem_cleanup) so protect */
        POSIX_ENSURE(initialized, S2N_ERR_NOT_INITIALIZED);
        POSIX_ENSURE(s2n_cleanup_atexit_impl(), S2N_ERR_ATEXIT);
    }

    return 0;
}

static void s2n_cleanup_atexit(void)
{
    (void) s2n_cleanup_atexit_impl();
}
