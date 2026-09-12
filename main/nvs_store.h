/*
 * nvs_store.h - NVS binding of saf_core's storage interface.
 * docs/nvs-queue.md explains the wear story: one blob per advancing
 * key, so NVS's own log-structured wear leveling spreads the queue
 * over the partition instead of hammering one entry.
 */
#ifndef NVS_STORE_H
#define NVS_STORE_H

#include "saf_core.h"

/* Opens (and if needed initializes) NVS and fills the storage slots of
 * *ops. Aborts on unrecoverable NVS errors - a demo device without its
 * queue store has nothing to demonstrate. */
void nvs_store_init(saf_ops *ops);

#endif /* NVS_STORE_H */
