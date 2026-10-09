/* sg-control -- restore points (restore.c): what sg-snapshot publishes.
 *
 * Copyright (C) 2026 Stained Glass OS contributors
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */
#ifndef SG_RESTORE_H
#define SG_RESTORE_H

#define RP_MAX_SNAP 8
#define RP_MAX_PROB 8
struct rp_snap { WCHAR id[32], when[32], kind[16], label[300]; BOOL bootable; };
struct rp_status {
    BOOL ok;                            /* the status was there and whole */
    WCHAR fs[16];                       /* the root file system: btrfs, ext4... */
    BOOL layout;                        /* btrfs with @ (restore points) */
    WCHAR booted[32];                   /* the restore point running now ("" : the system) */
    BOOL pending_rollback, pending_undo, saved, ready, has_undo;
    WCHAR convert[32], convert_detail[256], wentback[32];
    int n;                              /* restore points, newest first */
    struct rp_snap snap[RP_MAX_SNAP];
    WCHAR undo_id[32], undo_when[32], undo_label[300];
    WCHAR undone_when[32];
    int undone;                         /* -1 none, 0 failed, 1 done */
    int nprob;
    WCHAR prob[RP_MAX_PROB][256];
};
BOOL rp_read(struct rp_status *s);
BOOL rp_convert_offered(const struct rp_status *s);
BOOL rp_convert_unconfirmed(const struct rp_status *s);
BOOL rp_elevated(const WCHAR *sub, const WCHAR *id);
int  rp_admin(int argc, WCHAR **argv);
void rp_restart(const WCHAR *what);
void dump_recovery(void);

#endif
