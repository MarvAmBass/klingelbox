/* mbedtls/ecp.h - host-test stub. See mbedtls/pk.h for the model. The
 * keypair carries only a generation number: gen_key mints a fresh one, and
 * the PEM writers turn it into the pair tag the fake check_pair compares. */
#ifndef DB_HOSTTEST_MBEDTLS_ECP_H
#define DB_HOSTTEST_MBEDTLS_ECP_H

#include <stddef.h>

typedef enum {
    MBEDTLS_ECP_DP_SECP256R1 = 1,
} mbedtls_ecp_group_id;

typedef struct mbedtls_ecp_keypair {
    int gen_id;   /* 0 = never generated */
} mbedtls_ecp_keypair;

int mbedtls_ecp_gen_key(mbedtls_ecp_group_id grp_id, mbedtls_ecp_keypair *key,
                        int (*f_rng)(void *, unsigned char *, size_t),
                        void *p_rng);

#endif
