/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/smfs/kml.c
 *  Lustre filesystem abstraction routines
 *
 *  Copyright (C) 2004 Cluster File Systems, Inc.
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */
#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#define DEBUG_SUBSYSTEM S_SM

#include <linux/kmod.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/obd_class.h>
#include <linux/obd_support.h>
#include <linux/lustre_lib.h>
#include <linux/lustre_idl.h>
#include <linux/lustre_fsfilt.h>
#include <linux/lustre_smfs.h>
#include "smfs_internal.h"

smfs_pack_rec_func smfs_get_rec_pack_type(struct super_block *sb)
{
        struct smfs_super_info *smsi = S2SMI(sb);

        int index = GET_REC_PACK_TYPE_INDEX(smsi->smsi_flags);

        return smsi->smsi_pack_rec[index];
}
static int smfs_rec_post_hook(struct inode *inode, struct dentry *dentry,
                              void *data1, void *data2, int op, void *handle)
{
        int rc = 0;
        ENTRY;

        if (smfs_do_rec(inode))                                  
                rc = smfs_post_kml_rec(inode, dentry, data1, data2, op);  
        
        RETURN(rc);
}

#define KML_HOOK "kml_hook"
int smfs_rec_init(struct super_block *sb)
{
        struct smfs_super_info *smfs_info = S2SMI(sb);
        struct smfs_hook_ops   *rec_hops = NULL;
        int rc = 0;

        SMFS_SET_REC(smfs_info);

        ost_rec_pack_init(sb);
        mds_rec_pack_init(sb);

        rec_hops = smfs_alloc_hook_ops(KML_HOOK, NULL, smfs_rec_post_hook);
        if (!rec_hops) {
                RETURN(-ENOMEM);
        }
 
        rc = smfs_register_hook_ops(sb, rec_hops);      

        if (rc && rec_hops) {
                smfs_unregister_hook_ops(sb, rec_hops->smh_name);
                smfs_free_hook_ops(rec_hops);
        } 
        RETURN(rc);
}

int smfs_rec_cleanup(struct super_block *sb)
{
        struct smfs_hook_ops *rec_hops; 
        int rc = 0;

        rec_hops = smfs_unregister_hook_ops(sb, KML_HOOK);
        smfs_free_hook_ops(rec_hops);
        SMFS_CLEAN_REC(S2SMI(sb));
        
        RETURN(rc);
}

static inline void copy_inode_attr(struct iattr *iattr, struct inode *inode)
{
        iattr->ia_mode = inode->i_mode;
        iattr->ia_uid  = inode->i_uid;
        iattr->ia_gid  = inode->i_gid;
        iattr->ia_atime = inode->i_atime;
        iattr->ia_ctime = inode->i_ctime;
        iattr->ia_mtime = inode->i_mtime;
        iattr->ia_size = inode->i_size;
}

static inline int unpack_rec_data(char **p_buffer, int *size,
                                  char *in_data, char *args_data)
{
        int args_len = 0;
        int rc = 0;

        if (args_data)
                args_len = strlen(args_data);

        *size = *((int*)(in_data));
        rc = *size + sizeof(int);

        OBD_ALLOC(*p_buffer, *size + args_len + 1);
        if (!*p_buffer)
                RETURN(-ENOMEM);
        /*First copy reint dir */
        if (args_data)
                memcpy(*p_buffer, args_data, args_len);

        /*then copy the node name */
        memcpy(*p_buffer + args_len,
                      (in_data + sizeof(int)), *size);

        *size += args_len;

        RETURN(rc);
}

int smfs_rec_unpack(struct smfs_proc_args *args, char *record, 
                    char **pbuf, int *opcode)
{
        int offset = *(int *)(record);
        char *tmp = record + offset + sizeof(int);
        int rc = 0;
        *opcode = *(int *)tmp;
        *pbuf = tmp + sizeof(*opcode);
        RETURN(rc);
}
EXPORT_SYMBOL(smfs_rec_unpack);

int smfs_start_rec(struct super_block *sb, struct vfsmount *mnt)
{
        struct dentry *dentry;
        struct lvfs_run_ctxt saved;
        int rc = 0;
        ENTRY;

        if (SMFS_INIT_REC(S2SMI(sb)) ||
            (!SMFS_DO_REC(S2SMI(sb)) && !SMFS_CACHE_HOOK(S2SMI(sb))))
                RETURN(rc);
        
        rc = smfs_llog_setup(sb, mnt);
        if (rc)
                RETURN(rc); 
        push_ctxt(&saved, S2SMI(sb)->smsi_ctxt, NULL);
        dentry = simple_mkdir(current->fs->pwd, "DELETE", 0777, 1);
        if (IS_ERR(dentry)) {
                rc = PTR_ERR(dentry);
                CERROR("cannot create DELETE directory: rc = %d\n", rc);
                GOTO(err_exit, rc = -EINVAL);
        }
        S2SMI(sb)->smsi_delete_dir = dentry;

        if (!rc)
                SMFS_SET_INIT_REC(S2SMI(sb));
exit:
        pop_ctxt(&saved, S2SMI(sb)->smsi_ctxt, NULL);
        RETURN(rc);
err_exit:
        if (S2SMI(sb)->smsi_ctxt)
                OBD_FREE(S2SMI(sb)->smsi_ctxt, sizeof(struct lvfs_run_ctxt));
        goto exit;
}
EXPORT_SYMBOL(smfs_start_rec);

int smfs_post_setup(struct super_block *sb, struct vfsmount *mnt)
{
        struct lvfs_run_ctxt *current_ctxt = NULL;
        struct smfs_super_info *smb = S2SMI(sb);
 
        OBD_ALLOC(current_ctxt, sizeof(*current_ctxt));
        if (!current_ctxt)
                RETURN(-ENOMEM);
        OBD_SET_CTXT_MAGIC(current_ctxt);
        
        current_ctxt->pwdmnt = mnt;
        current_ctxt->pwd = mnt->mnt_root;
        current_ctxt->fs = get_ds();
        smb->smsi_ctxt = current_ctxt;

        RETURN(0);
}
EXPORT_SYMBOL(smfs_post_setup);

int smfs_post_cleanup(struct super_block *sb)
{
        struct smfs_super_info *smb = S2SMI(sb);
        
        ENTRY;
       
        if (smb->smsi_ctxt)
                OBD_FREE(S2SMI(sb)->smsi_ctxt, sizeof(struct lvfs_run_ctxt));
        RETURN(0);
}
EXPORT_SYMBOL(smfs_post_cleanup);

int smfs_stop_rec(struct super_block *sb)
{
        int rc = 0;

        if (!SMFS_INIT_REC(S2SMI(sb)) ||
            (!SMFS_DO_REC(S2SMI(sb)) && !SMFS_CACHE_HOOK(S2SMI(sb))))
                RETURN(rc);

        rc = smfs_llog_cleanup(sb);

        SMFS_CLEAN_INIT_REC(S2SMI(sb));

        if (S2SMI(sb)->smsi_delete_dir) {
                l_dput(S2SMI(sb)->smsi_delete_dir);
                S2SMI(sb)->smsi_delete_dir = NULL;
        }
        RETURN(rc);
}
EXPORT_SYMBOL(smfs_stop_rec);

int smfs_write_extents(struct inode *dir, struct dentry *dentry,
                       unsigned long from, unsigned long num)
{
        return smfs_post_rec_write(dir, dentry, &from, &num);
}
EXPORT_SYMBOL(smfs_write_extents);

int smfs_rec_setattr(struct inode *dir, struct dentry *dentry,
                     struct iattr *attr)
{
        return smfs_post_rec_setattr(dir, dentry, attr, NULL);
}
EXPORT_SYMBOL(smfs_rec_setattr);

int smfs_rec_md(struct inode *inode, void * lmm, int lmm_size)
{
        char *set_lmm = NULL;
        int  rc = 0;
        ENTRY;

        if (lmm) {
                OBD_ALLOC(set_lmm, lmm_size + sizeof(lmm_size));
                if (!set_lmm)
                        RETURN(-ENOMEM);
                memcpy(set_lmm, &lmm_size, sizeof(lmm_size));
                memcpy(set_lmm + sizeof(lmm_size), lmm, lmm_size);
                rc = smfs_post_rec_setattr(inode, NULL, NULL, set_lmm);
                if (rc) {
                        CERROR("Error: Record md for inode %lu rc=%d\n",
                                inode->i_ino, rc);
                }
        }
        if (set_lmm)
                OBD_FREE(set_lmm, lmm_size + sizeof(lmm_size));
        return rc;
}
EXPORT_SYMBOL(smfs_rec_md);

int smfs_rec_precreate(struct dentry *dentry, int *num, struct obdo *oa)
{
       return smfs_post_rec_create(dentry->d_inode, dentry, num, oa);
}
EXPORT_SYMBOL(smfs_rec_precreate);

int smfs_process_rec(struct super_block *sb,
                     int count, char *dir, int flags)
{
        struct llog_ctxt *ctxt;
        struct llog_handle *loghandle;
        struct smfs_proc_args args;
        int rc = 0;

        if (!SMFS_INIT_REC(S2SMI(sb))) {
                CWARN("Did not start up rec server \n");
                RETURN(rc);
        }

        memset(&args, 0, sizeof(struct smfs_proc_args));
        args.sr_sb = sb;
        args.sr_count = count;
        args.sr_data = dir;
        args.sr_flags = flags ;
        ctxt = S2SMI(sb)->smsi_rec_log;
        loghandle = ctxt->loc_handle;

        if (count == 0) {
                if (SMFS_DO_REINT_REC(flags)) {
                        struct llog_gen_rec *lgr;

                        /*For reint rec, we need insert
                          *a gen rec to identify the end
                          *of the rec.*/
                        OBD_ALLOC(lgr, sizeof(*lgr));
                        if (!lgr)
                                RETURN(-ENOMEM);
                        lgr->lgr_hdr.lrh_len = lgr->lgr_tail.lrt_len = sizeof(*lgr);
                        lgr->lgr_hdr.lrh_type = LLOG_GEN_REC;
                        lgr->lgr_gen = ctxt->loc_gen;
                        rc = llog_add(ctxt, &lgr->lgr_hdr, NULL, NULL, 1,
                                      NULL, NULL, NULL);
                        OBD_FREE(lgr, sizeof(*lgr));
                        if (rc != 1)
                                RETURN(rc);
                }
        } else {
                SET_REC_COUNT_FLAGS(args.sr_flags, SMFS_REC_ALL);
        }
        if (loghandle) {
                if (SMFS_DO_REINT_REC(flags))
                        rc = llog_cat_process(loghandle, ctxt->llog_proc_cb,
                                              (void *)&args);
                else
                        rc = llog_cat_reverse_process(loghandle,
                                                      ctxt->llog_proc_cb,
                                                      (void *)&args);
                if (rc == LLOG_PROC_BREAK)
                        rc = 0;
        }
        RETURN(rc);
}
