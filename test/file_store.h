/*
 * file_store.h - file-backed implementation of saf_core's storage
 * interface, one file per record key, standing in for NVS on the host.
 *
 * Its distinguishing feature is the power-cut fault: with
 * SAF_TORN_AT_PUT=<n> in the environment, the n-th store_put of this
 * process writes only the first half of the record, fills the rest
 * with 0xFF (how flash reads back a page the cut never finished
 * programming - same length, garbage tail, only the CRC knows), then
 * _exit()s mid-operation. The 02/03 control-first doctrine applies:
 * chaos proves this fault DEFEATS a device with the CRC check stubbed
 * out before trusting the device that detects it.
 */
#ifndef FILE_STORE_H
#define FILE_STORE_H

#include "../main/saf_core.h"

#define FILE_STORE_TORN_EXIT 37 /* the "power died mid-write" exit code */

typedef struct {
    const char *dir;   /* must exist */
    int puts_seen;     /* counts store_put calls for the torn trigger */
    int torn_at;       /* from SAF_TORN_AT_PUT; 0 = never */
} file_store;

/* Reads SAF_TORN_AT_PUT and fills *ops with the file-backed callbacks;
 * pass fs as the ops context. */
void file_store_init(file_store *fs, const char *dir, saf_ops *ops);

#endif /* FILE_STORE_H */
