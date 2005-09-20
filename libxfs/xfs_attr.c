/*
 * Copyright (c) 2005 Silicon Graphics, Inc.  All Rights Reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it would be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * Further, this software is distributed without any warranty that it is
 * free of the rightful claim of any third person regarding infringement
 * or the like.  Any license provided herein, whether implied or
 * otherwise, applies only to this software file.  Patent licenses, if
 * any, provided herein do not apply to combinations of this program with
 * other software, or any other product whatsoever.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston MA 02111-1307, USA.
 *
 * Contact information: Silicon Graphics, Inc., 1600 Amphitheatre Pkwy,
 * Mountain View, CA  94043, or:
 *
 * http://www.sgi.com
 *
 * For further information regarding this notice, see:
 *
 * http://oss.sgi.com/projects/GenInfo/SGIGPLNoticeExplan/
 */

#include <xfs.h>

/*
 * xfs_attr.c
 *
 * Provide the external interfaces to manage attribute lists.
 */

/*========================================================================
 * Function prototypes for the kernel.
 *========================================================================*/

/*
 * Internal routines when attribute list fits inside the inode.
 */
STATIC int xfs_attr_shortform_addname(xfs_da_args_t *args);

/*
 * Internal routines when attribute list is one block.
 */
STATIC int xfs_attr_leaf_get(xfs_da_args_t *args);
STATIC int xfs_attr_leaf_addname(xfs_da_args_t *args);
STATIC int xfs_attr_leaf_removename(xfs_da_args_t *args);
STATIC int xfs_attr_leaf_list(xfs_attr_list_context_t *context);

/*
 * Internal routines when attribute list is more than one block.
 */
STATIC int xfs_attr_node_get(xfs_da_args_t *args);
STATIC int xfs_attr_node_addname(xfs_da_args_t *args);
STATIC int xfs_attr_node_removename(xfs_da_args_t *args);
STATIC int xfs_attr_node_list(xfs_attr_list_context_t *context);
STATIC int xfs_attr_fillstate(xfs_da_state_t *state);
STATIC int xfs_attr_refillstate(xfs_da_state_t *state);

/*
 * Routines to manipulate out-of-line attribute values.
 */
STATIC int xfs_attr_rmtval_get(xfs_da_args_t *args);
STATIC int xfs_attr_rmtval_set(xfs_da_args_t *args);
STATIC int xfs_attr_rmtval_remove(xfs_da_args_t *args);

#define ATTR_RMTVALUE_MAPSIZE	1	/* # of map entries at once */

/*========================================================================
 * Overall external interface routines.
 *========================================================================*/

int
xfs_attr_set_int(xfs_inode_t *dp, char *name, int namelen,
		 char *value, int valuelen, int flags)
{
	xfs_da_args_t	args;
	xfs_fsblock_t	firstblock;
	xfs_bmap_free_t flist;
	int		error, err2, committed;
	int		local, size;
	uint		nblks;
	xfs_mount_t	*mp = dp->i_mount;
	int             rsvd = (flags & ATTR_ROOT) != 0;

	/*
	 * Attach the dquots to the inode.
	 */
	if ((error = XFS_QM_DQATTACH(mp, dp, 0)))
		return (error);

	/*
	 * Determine space new attribute will use, and if it would be
	 * "local" or "remote" (note: local != inline).
	 */
	size = xfs_attr_leaf_newentsize(namelen, valuelen,
					mp->m_sb.sb_blocksize, &local);

	/*
	 * If the inode doesn't have an attribute fork, add one.
	 * (inode must not be locked when we call this routine)
	 */
	if (XFS_IFORK_Q(dp) == 0) {
		if ((error = xfs_bmap_add_attrfork(dp, size, rsvd)))
			return(error);
	}

	/*
	 * Fill in the arg structure for this request.
	 */
	memset((char *)&args, 0, sizeof(args));
	args.name = name;
	args.namelen = namelen;
	args.value = value;
	args.valuelen = valuelen;
	args.flags = flags;
	args.hashval = xfs_da_hashname(args.name, args.namelen);
	args.dp = dp;
	args.firstblock = &firstblock;
	args.flist = &flist;
	args.whichfork = XFS_ATTR_FORK;
	args.addname = 1;
	args.oknoent = 1;

	nblks = XFS_DAENTER_SPACE_RES(mp, XFS_ATTR_FORK);
	if (local) {
		if (size > (mp->m_sb.sb_blocksize >> 1)) {
			/* Double split possible */
			nblks <<= 1;
		}
	} else {
		uint	dblocks = XFS_B_TO_FSB(mp, valuelen);
		/* Out of line attribute, cannot double split, but make
		 * room for the attribute value itself.
		 */
		nblks += dblocks;
		nblks += XFS_NEXTENTADD_SPACE_RES(mp, dblocks, XFS_ATTR_FORK);
	}

	/* Size is now blocks for attribute data */
	args.total = nblks;

	/*
	 * Start our first transaction of the day.
	 *
	 * All future transactions during this code must be "chained" off
	 * this one via the trans_dup() call.  All transactions will contain
	 * the inode, and the inode will always be marked with trans_ihold().
	 * Since the inode will be locked in all transactions, we must log
	 * the inode in every transaction to let it float upward through
	 * the log.
	 */
	args.trans = xfs_trans_alloc(mp, XFS_TRANS_ATTR_SET);

	/*
	 * Root fork attributes can use reserved data blocks for this
	 * operation if necessary
	 */

	if (rsvd)
		args.trans->t_flags |= XFS_TRANS_RESERVE;

	if ((error = xfs_trans_reserve(args.trans, (uint) nblks,
				      XFS_ATTRSET_LOG_RES(mp, nblks),
				      0, XFS_TRANS_PERM_LOG_RES,
				      XFS_ATTRSET_LOG_COUNT))) {
		xfs_trans_cancel(args.trans, 0);
		return(error);
	}
	xfs_ilock(dp, XFS_ILOCK_EXCL);

	error = XFS_TRANS_RESERVE_QUOTA_NBLKS(mp, args.trans, dp, nblks, 0,
			 rsvd ? XFS_QMOPT_RES_REGBLKS | XFS_QMOPT_FORCE_RES :
				XFS_QMOPT_RES_REGBLKS);
	if (error) {
		xfs_iunlock(dp, XFS_ILOCK_EXCL);
		xfs_trans_cancel(args.trans, XFS_TRANS_RELEASE_LOG_RES);
		return (error);
	}

	xfs_trans_ijoin(args.trans, dp, XFS_ILOCK_EXCL);
	xfs_trans_ihold(args.trans, dp);

	/*
	 * If the attribute list is non-existant or a shortform list,
	 * upgrade it to a single-leaf-block attribute list.
	 */
	if ((dp->i_d.di_aformat == XFS_DINODE_FMT_LOCAL) ||
	    ((dp->i_d.di_aformat == XFS_DINODE_FMT_EXTENTS) &&
	     (dp->i_d.di_anextents == 0))) {

		/*
		 * Build initial attribute list (if required).
		 */
		if (dp->i_d.di_aformat == XFS_DINODE_FMT_EXTENTS)
			xfs_attr_shortform_create(&args);

		/*
		 * Try to add the attr to the attribute list in
		 * the inode.
		 */
		error = xfs_attr_shortform_addname(&args);
		if (error != ENOSPC) {
			/*
			 * Commit the shortform mods, and we're done.
			 * NOTE: this is also the error path (EEXIST, etc).
			 */
			ASSERT(args.trans != NULL);

			/*
			 * If this is a synchronous mount, make sure that
			 * the transaction goes to disk before returning
			 * to the user.
			 */
			if (mp->m_flags & XFS_MOUNT_WSYNC) {
				xfs_trans_set_sync(args.trans);
			}
			err2 = xfs_trans_commit(args.trans,
						 XFS_TRANS_RELEASE_LOG_RES,
						 NULL);
			xfs_iunlock(dp, XFS_ILOCK_EXCL);

			/*
			 * Hit the inode change time.
			 */
			if (!error && (flags & ATTR_KERNOTIME) == 0) {
				xfs_ichgtime(dp, XFS_ICHGTIME_CHG);
			}
			return(error == 0 ? err2 : error);
		}

		/*
		 * It won't fit in the shortform, transform to a leaf block.
		 * GROT: another possible req'mt for a double-split btree op.
		 */
		XFS_BMAP_INIT(args.flist, args.firstblock);
		error = xfs_attr_shortform_to_leaf(&args);
		if (!error) {
			error = xfs_bmap_finish(&args.trans, args.flist,
						*args.firstblock, &committed);
		}
		if (error) {
			ASSERT(committed);
			args.trans = NULL;
			xfs_bmap_cancel(&flist);
			goto out;
		}

		/*
		 * bmap_finish() may have committed the last trans and started
		 * a new one.  We need the inode to be in all transactions.
		 */
		if (committed) {
			xfs_trans_ijoin(args.trans, dp, XFS_ILOCK_EXCL);
			xfs_trans_ihold(args.trans, dp);
		}

		/*
		 * Commit the leaf transformation.  We'll need another (linked)
		 * transaction to add the new attribute to the leaf.
		 */
		if ((error = xfs_attr_rolltrans(&args.trans, dp)))
			goto out;

	}

	if (xfs_bmap_one_block(dp, XFS_ATTR_FORK)) {
		error = xfs_attr_leaf_addname(&args);
	} else {
		error = xfs_attr_node_addname(&args);
	}
	if (error) {
		goto out;
	}

	/*
	 * If this is a synchronous mount, make sure that the
	 * transaction goes to disk before returning to the user.
	 */
	if (mp->m_flags & XFS_MOUNT_WSYNC) {
		xfs_trans_set_sync(args.trans);
	}

	/*
	 * Commit the last in the sequence of transactions.
	 */
	xfs_trans_log_inode(args.trans, dp, XFS_ILOG_CORE);
	error = xfs_trans_commit(args.trans, XFS_TRANS_RELEASE_LOG_RES,
				 NULL);
	xfs_iunlock(dp, XFS_ILOCK_EXCL);

	/*
	 * Hit the inode change time.
	 */
	if (!error && (flags & ATTR_KERNOTIME) == 0) {
		xfs_ichgtime(dp, XFS_ICHGTIME_CHG);
	}

	return(error);

out:
	if (args.trans)
		xfs_trans_cancel(args.trans,
			XFS_TRANS_RELEASE_LOG_RES|XFS_TRANS_ABORT);
	xfs_iunlock(dp, XFS_ILOCK_EXCL);
	return(error);
}

STATIC int
xfs_attr_remove_int(xfs_inode_t *dp, char *name, int namelen, int flags)
{
	xfs_da_args_t	args;
	xfs_fsblock_t	firstblock;
	xfs_bmap_free_t	flist;
	int		error;
	xfs_mount_t	*mp = dp->i_mount;

	/*
	 * Fill in the arg structure for this request.
	 */
	memset((char *)&args, 0, sizeof(args));
	args.name = name;
	args.namelen = namelen;
	args.flags = flags;
	args.hashval = xfs_da_hashname(args.name, args.namelen);
	args.dp = dp;
	args.firstblock = &firstblock;
	args.flist = &flist;
	args.total = 0;
	args.whichfork = XFS_ATTR_FORK;

	/*
	 * Attach the dquots to the inode.
	 */
	if ((error = XFS_QM_DQATTACH(mp, dp, 0)))
		return (error);

	/*
	 * Start our first transaction of the day.
	 *
	 * All future transactions during this code must be "chained" off
	 * this one via the trans_dup() call.  All transactions will contain
	 * the inode, and the inode will always be marked with trans_ihold().
	 * Since the inode will be locked in all transactions, we must log
	 * the inode in every transaction to let it float upward through
	 * the log.
	 */
	args.trans = xfs_trans_alloc(mp, XFS_TRANS_ATTR_RM);

	/*
	 * Root fork attributes can use reserved data blocks for this
	 * operation if necessary
	 */

	if (flags & ATTR_ROOT)
		args.trans->t_flags |= XFS_TRANS_RESERVE;

	if ((error = xfs_trans_reserve(args.trans,
				      XFS_ATTRRM_SPACE_RES(mp),
				      XFS_ATTRRM_LOG_RES(mp),
				      0, XFS_TRANS_PERM_LOG_RES,
				      XFS_ATTRRM_LOG_COUNT))) {
		xfs_trans_cancel(args.trans, 0);
		return(error);
	}

	xfs_ilock(dp, XFS_ILOCK_EXCL);
	/*
	 * No need to make quota reservations here. We expect to release some
	 * blocks not allocate in the common case.
	 */
	xfs_trans_ijoin(args.trans, dp, XFS_ILOCK_EXCL);
	xfs_trans_ihold(args.trans, dp);

	/*
	 * Decide on what work routines to call based on the inode size.
	 */
	if (XFS_IFORK_Q(dp) == 0 ||
	    (dp->i_d.di_aformat == XFS_DINODE_FMT_EXTENTS &&
	     dp->i_d.di_anextents == 0)) {
		error = XFS_ERROR(ENOATTR);
		goto out;
	}
	if (dp->i_d.di_aformat == XFS_DINODE_FMT_LOCAL) {
		ASSERT(dp->i_afp->if_flags & XFS_IFINLINE);
		error = xfs_attr_shortform_remove(&args);
		if (error) {
			goto out;
		}
	} else if (xfs_bmap_one_block(dp, XFS_ATTR_FORK)) {
		error = xfs_attr_leaf_removename(&args);
	} else {
		error = xfs_attr_node_removename(&args);
	}
	if (error) {
		goto out;
	}

	/*
	 * If this is a synchronous mount, make sure that the
	 * transaction goes to disk before returning to the user.
	 */
	if (mp->m_flags & XFS_MOUNT_WSYNC) {
		xfs_trans_set_sync(args.trans);
	}

	/*
	 * Commit the last in the sequence of transactions.
	 */
	xfs_trans_log_inode(args.trans, dp, XFS_ILOG_CORE);
	error = xfs_trans_commit(args.trans, XFS_TRANS_RELEASE_LOG_RES,
				 NULL);
	xfs_iunlock(dp, XFS_ILOCK_EXCL);

	/*
	 * Hit the inode change time.
	 */
	if (!error && (flags & ATTR_KERNOTIME) == 0) {
		xfs_ichgtime(dp, XFS_ICHGTIME_CHG);
	}

	return(error);

out:
	if (args.trans)
		xfs_trans_cancel(args.trans,
			XFS_TRANS_RELEASE_LOG_RES|XFS_TRANS_ABORT);
	xfs_iunlock(dp, XFS_ILOCK_EXCL);
	return(error);
}


/*========================================================================
 * External routines when attribute list is inside the inode
 *========================================================================*/

/*
 * Add a name to the shortform attribute list structure
 * This is the external routine.
 */
STATIC int
xfs_attr_shortform_addname(xfs_da_args_t *args)
{
	int newsize, forkoff, retval;

	retval = xfs_attr_shortform_lookup(args);
	if ((args->flags & ATTR_REPLACE) && (retval == ENOATTR)) {
		return(retval);
	} else if (retval == EEXIST) {
		if (args->flags & ATTR_CREATE)
			return(retval);
		retval = xfs_attr_shortform_remove(args);
		ASSERT(retval == 0);
	}

	if (args->namelen >= XFS_ATTR_SF_ENTSIZE_MAX ||
	    args->valuelen >= XFS_ATTR_SF_ENTSIZE_MAX)
		return(XFS_ERROR(ENOSPC));

	newsize = XFS_ATTR_SF_TOTSIZE(args->dp);
	newsize += XFS_ATTR_SF_ENTSIZE_BYNAME(args->namelen, args->valuelen);

	forkoff = xfs_attr_shortform_bytesfit(args->dp, newsize);
	if (!forkoff)
		return(XFS_ERROR(ENOSPC));

	xfs_attr_shortform_add(args, forkoff);
	return(0);
}


/*========================================================================
 * External routines when attribute list is one block
 *========================================================================*/

/*
 * Add a name to the leaf attribute list structure
 *
 * This leaf block cannot have a "remote" value, we only call this routine
 * if bmap_one_block() says there is only one block (ie: no remote blks).
 */
int
xfs_attr_leaf_addname(xfs_da_args_t *args)
{
	xfs_inode_t *dp;
	xfs_dabuf_t *bp;
	int retval, error, committed, forkoff;

	/*
	 * Read the (only) block in the attribute list in.
	 */
	dp = args->dp;
	args->blkno = 0;
	error = xfs_da_read_buf(args->trans, args->dp, args->blkno, -1, &bp,
					     XFS_ATTR_FORK);
	if (error)
		return(error);
	ASSERT(bp != NULL);

	/*
	 * Look up the given attribute in the leaf block.  Figure out if
	 * the given flags produce an error or call for an atomic rename.
	 */
	retval = xfs_attr_leaf_lookup_int(bp, args);
	if ((args->flags & ATTR_REPLACE) && (retval == ENOATTR)) {
		xfs_da_brelse(args->trans, bp);
		return(retval);
	} else if (retval == EEXIST) {
		if (args->flags & ATTR_CREATE) {	/* pure create op */
			xfs_da_brelse(args->trans, bp);
			return(retval);
		}
		args->rename = 1;			/* an atomic rename */
		args->blkno2 = args->blkno;		/* set 2nd entry info*/
		args->index2 = args->index;
		args->rmtblkno2 = args->rmtblkno;
		args->rmtblkcnt2 = args->rmtblkcnt;
	}

	/*
	 * Add the attribute to the leaf block, transitioning to a Btree
	 * if required.
	 */
	retval = xfs_attr_leaf_add(bp, args);
	xfs_da_buf_done(bp);
	if (retval == ENOSPC) {
		/*
		 * Promote the attribute list to the Btree format, then
		 * Commit that transaction so that the node_addname() call
		 * can manage its own transactions.
		 */
		XFS_BMAP_INIT(args->flist, args->firstblock);
		error = xfs_attr_leaf_to_node(args);
		if (!error) {
			error = xfs_bmap_finish(&args->trans, args->flist,
						*args->firstblock, &committed);
		}
		if (error) {
			ASSERT(committed);
			args->trans = NULL;
			xfs_bmap_cancel(args->flist);
			return(error);
		}

		/*
		 * bmap_finish() may have committed the last trans and started
		 * a new one.  We need the inode to be in all transactions.
		 */
		if (committed) {
			xfs_trans_ijoin(args->trans, dp, XFS_ILOCK_EXCL);
			xfs_trans_ihold(args->trans, dp);
		}

		/*
		 * Commit the current trans (including the inode) and start
		 * a new one.
		 */
		if ((error = xfs_attr_rolltrans(&args->trans, dp)))
			return (error);

		/*
		 * Fob the whole rest of the problem off on the Btree code.
		 */
		error = xfs_attr_node_addname(args);
		return(error);
	}

	/*
	 * Commit the transaction that added the attr name so that
	 * later routines can manage their own transactions.
	 */
	if ((error = xfs_attr_rolltrans(&args->trans, dp)))
		return (error);

	/*
	 * If there was an out-of-line value, allocate the blocks we
	 * identified for its storage and copy the value.  This is done
	 * after we create the attribute so that we don't overflow the
	 * maximum size of a transaction and/or hit a deadlock.
	 */
	if (args->rmtblkno > 0) {
		error = xfs_attr_rmtval_set(args);
		if (error)
			return(error);
	}

	/*
	 * If this is an atomic rename operation, we must "flip" the
	 * incomplete flags on the "new" and "old" attribute/value pairs
	 * so that one disappears and one appears atomically.  Then we
	 * must remove the "old" attribute/value pair.
	 */
	if (args->rename) {
		/*
		 * In a separate transaction, set the incomplete flag on the
		 * "old" attr and clear the incomplete flag on the "new" attr.
		 */
		error = xfs_attr_leaf_flipflags(args);
		if (error)
			return(error);

		/*
		 * Dismantle the "old" attribute/value pair by removing
		 * a "remote" value (if it exists).
		 */
		args->index = args->index2;
		args->blkno = args->blkno2;
		args->rmtblkno = args->rmtblkno2;
		args->rmtblkcnt = args->rmtblkcnt2;
		if (args->rmtblkno) {
			error = xfs_attr_rmtval_remove(args);
			if (error)
				return(error);
		}

		/*
		 * Read in the block containing the "old" attr, then
		 * remove the "old" attr from that block (neat, huh!)
		 */
		error = xfs_da_read_buf(args->trans, args->dp, args->blkno, -1,
						     &bp, XFS_ATTR_FORK);
		if (error)
			return(error);
		ASSERT(bp != NULL);
		(void)xfs_attr_leaf_remove(bp, args);

		/*
		 * If the result is small enough, shrink it all into the inode.
		 */
		if ((forkoff = xfs_attr_shortform_allfit(bp, dp))) {
			XFS_BMAP_INIT(args->flist, args->firstblock);
			error = xfs_attr_leaf_to_shortform(bp, args, forkoff);
			/* bp is gone due to xfs_da_shrink_inode */
			if (!error) {
				error = xfs_bmap_finish(&args->trans,
							args->flist,
							*args->firstblock,
							&committed);
			}
			if (error) {
				ASSERT(committed);
				args->trans = NULL;
				xfs_bmap_cancel(args->flist);
				return(error);
			}

			/*
			 * bmap_finish() may have committed the last trans
			 * and started a new one.  We need the inode to be
			 * in all transactions.
			 */
			if (committed) {
				xfs_trans_ijoin(args->trans, dp, XFS_ILOCK_EXCL);
				xfs_trans_ihold(args->trans, dp);
			}
		} else
			xfs_da_buf_done(bp);

		/*
		 * Commit the remove and start the next trans in series.
		 */
		error = xfs_attr_rolltrans(&args->trans, dp);

	} else if (args->rmtblkno > 0) {
		/*
		 * Added a "remote" value, just clear the incomplete flag.
		 */
		error = xfs_attr_leaf_clearflag(args);
	}
	return(error);
}

/*
 * Remove a name from the leaf attribute list structure
 *
 * This leaf block cannot have a "remote" value, we only call this routine
 * if bmap_one_block() says there is only one block (ie: no remote blks).
 */
STATIC int
xfs_attr_leaf_removename(xfs_da_args_t *args)
{
	xfs_inode_t *dp;
	xfs_dabuf_t *bp;
	int error, committed, forkoff;

	/*
	 * Remove the attribute.
	 */
	dp = args->dp;
	args->blkno = 0;
	error = xfs_da_read_buf(args->trans, args->dp, args->blkno, -1, &bp,
					     XFS_ATTR_FORK);
	if (error) {
		return(error);
	}

	ASSERT(bp != NULL);
	error = xfs_attr_leaf_lookup_int(bp, args);
	if (error == ENOATTR) {
		xfs_da_brelse(args->trans, bp);
		return(error);
	}

	(void)xfs_attr_leaf_remove(bp, args);

	/*
	 * If the result is small enough, shrink it all into the inode.
	 */
	if ((forkoff = xfs_attr_shortform_allfit(bp, dp))) {
		XFS_BMAP_INIT(args->flist, args->firstblock);
		error = xfs_attr_leaf_to_shortform(bp, args, forkoff);
		/* bp is gone due to xfs_da_shrink_inode */
		if (!error) {
			error = xfs_bmap_finish(&args->trans, args->flist,
						*args->firstblock, &committed);
		}
		if (error) {
			ASSERT(committed);
			args->trans = NULL;
			xfs_bmap_cancel(args->flist);
			return(error);
		}

		/*
		 * bmap_finish() may have committed the last trans and started
		 * a new one.  We need the inode to be in all transactions.
		 */
		if (committed) {
			xfs_trans_ijoin(args->trans, dp, XFS_ILOCK_EXCL);
			xfs_trans_ihold(args->trans, dp);
		}
	} else
		xfs_da_buf_done(bp);
	return(0);
}

/*========================================================================
 * External routines when attribute list size > XFS_LBSIZE(mp).
 *========================================================================*/

/*
 * Add a name to a Btree-format attribute list.
 *
 * This will involve walking down the Btree, and may involve splitting
 * leaf nodes and even splitting intermediate nodes up to and including
 * the root node (a special case of an intermediate node).
 *
 * "Remote" attribute values confuse the issue and atomic rename operations
 * add a whole extra layer of confusion on top of that.
 */
STATIC int
xfs_attr_node_addname(xfs_da_args_t *args)
{
	xfs_da_state_t *state;
	xfs_da_state_blk_t *blk;
	xfs_inode_t *dp;
	xfs_mount_t *mp;
	int committed, retval, error;

	/*
	 * Fill in bucket of arguments/results/context to carry around.
	 */
	dp = args->dp;
	mp = dp->i_mount;
restart:
	state = xfs_da_state_alloc();
	state->args = args;
	state->mp = mp;
	state->blocksize = state->mp->m_sb.sb_blocksize;
	state->node_ents = state->mp->m_attr_node_ents;

	/*
	 * Search to see if name already exists, and get back a pointer
	 * to where it should go.
	 */
	error = xfs_da_node_lookup_int(state, &retval);
	if (error)
		goto out;
	blk = &state->path.blk[ state->path.active-1 ];
	ASSERT(blk->magic == XFS_ATTR_LEAF_MAGIC);
	if ((args->flags & ATTR_REPLACE) && (retval == ENOATTR)) {
		goto out;
	} else if (retval == EEXIST) {
		if (args->flags & ATTR_CREATE)
			goto out;
		args->rename = 1;			/* atomic rename op */
		args->blkno2 = args->blkno;		/* set 2nd entry info*/
		args->index2 = args->index;
		args->rmtblkno2 = args->rmtblkno;
		args->rmtblkcnt2 = args->rmtblkcnt;
		args->rmtblkno = 0;
		args->rmtblkcnt = 0;
	}

	retval = xfs_attr_leaf_add(blk->bp, state->args);
	if (retval == ENOSPC) {
		if (state->path.active == 1) {
			/*
			 * Its really a single leaf node, but it had
			 * out-of-line values so it looked like it *might*
			 * have been a b-tree.
			 */
			xfs_da_state_free(state);
			XFS_BMAP_INIT(args->flist, args->firstblock);
			error = xfs_attr_leaf_to_node(args);
			if (!error) {
				error = xfs_bmap_finish(&args->trans,
							args->flist,
							*args->firstblock,
							&committed);
			}
			if (error) {
				ASSERT(committed);
				args->trans = NULL;
				xfs_bmap_cancel(args->flist);
				goto out;
			}

			/*
			 * bmap_finish() may have committed the last trans
			 * and started a new one.  We need the inode to be
			 * in all transactions.
			 */
			if (committed) {
				xfs_trans_ijoin(args->trans, dp, XFS_ILOCK_EXCL);
				xfs_trans_ihold(args->trans, dp);
			}

			/*
			 * Commit the node conversion and start the next
			 * trans in the chain.
			 */
			if ((error = xfs_attr_rolltrans(&args->trans, dp)))
				goto out;

			goto restart;
		}

		/*
		 * Split as many Btree elements as required.
		 * This code tracks the new and old attr's location
		 * in the index/blkno/rmtblkno/rmtblkcnt fields and
		 * in the index2/blkno2/rmtblkno2/rmtblkcnt2 fields.
		 */
		XFS_BMAP_INIT(args->flist, args->firstblock);
		error = xfs_da_split(state);
		if (!error) {
			error = xfs_bmap_finish(&args->trans, args->flist,
						*args->firstblock, &committed);
		}
		if (error) {
			ASSERT(committed);
			args->trans = NULL;
			xfs_bmap_cancel(args->flist);
			goto out;
		}

		/*
		 * bmap_finish() may have committed the last trans and started
		 * a new one.  We need the inode to be in all transactions.
		 */
		if (committed) {
			xfs_trans_ijoin(args->trans, dp, XFS_ILOCK_EXCL);
			xfs_trans_ihold(args->trans, dp);
		}
	} else {
		/*
		 * Addition succeeded, update Btree hashvals.
		 */
		xfs_da_fixhashpath(state, &state->path);
	}

	/*
	 * Kill the state structure, we're done with it and need to
	 * allow the buffers to come back later.
	 */
	xfs_da_state_free(state);
	state = NULL;

	/*
	 * Commit the leaf addition or btree split and start the next
	 * trans in the chain.
	 */
	if ((error = xfs_attr_rolltrans(&args->trans, dp)))
		goto out;

	/*
	 * If there was an out-of-line value, allocate the blocks we
	 * identified for its storage and copy the value.  This is done
	 * after we create the attribute so that we don't overflow the
	 * maximum size of a transaction and/or hit a deadlock.
	 */
	if (args->rmtblkno > 0) {
		error = xfs_attr_rmtval_set(args);
		if (error)
			return(error);
	}

	/*
	 * If this is an atomic rename operation, we must "flip" the
	 * incomplete flags on the "new" and "old" attribute/value pairs
	 * so that one disappears and one appears atomically.  Then we
	 * must remove the "old" attribute/value pair.
	 */
	if (args->rename) {
		/*
		 * In a separate transaction, set the incomplete flag on the
		 * "old" attr and clear the incomplete flag on the "new" attr.
		 */
		error = xfs_attr_leaf_flipflags(args);
		if (error)
			goto out;

		/*
		 * Dismantle the "old" attribute/value pair by removing
		 * a "remote" value (if it exists).
		 */
		args->index = args->index2;
		args->blkno = args->blkno2;
		args->rmtblkno = args->rmtblkno2;
		args->rmtblkcnt = args->rmtblkcnt2;
		if (args->rmtblkno) {
			error = xfs_attr_rmtval_remove(args);
			if (error)
				return(error);
		}

		/*
		 * Re-find the "old" attribute entry after any split ops.
		 * The INCOMPLETE flag means that we will find the "old"
		 * attr, not the "new" one.
		 */
		args->flags |= XFS_ATTR_INCOMPLETE;
		state = xfs_da_state_alloc();
		state->args = args;
		state->mp = mp;
		state->blocksize = state->mp->m_sb.sb_blocksize;
		state->node_ents = state->mp->m_attr_node_ents;
		state->inleaf = 0;
		error = xfs_da_node_lookup_int(state, &retval);
		if (error)
			goto out;

		/*
		 * Remove the name and update the hashvals in the tree.
		 */
		blk = &state->path.blk[ state->path.active-1 ];
		ASSERT(blk->magic == XFS_ATTR_LEAF_MAGIC);
		error = xfs_attr_leaf_remove(blk->bp, args);
		xfs_da_fixhashpath(state, &state->path);

		/*
		 * Check to see if the tree needs to be collapsed.
		 */
		if (retval && (state->path.active > 1)) {
			XFS_BMAP_INIT(args->flist, args->firstblock);
			error = xfs_da_join(state);
			if (!error) {
				error = xfs_bmap_finish(&args->trans,
							args->flist,
							*args->firstblock,
							&committed);
			}
			if (error) {
				ASSERT(committed);
				args->trans = NULL;
				xfs_bmap_cancel(args->flist);
				goto out;
			}

			/*
			 * bmap_finish() may have committed the last trans
			 * and started a new one.  We need the inode to be
			 * in all transactions.
			 */
			if (committed) {
				xfs_trans_ijoin(args->trans, dp, XFS_ILOCK_EXCL);
				xfs_trans_ihold(args->trans, dp);
			}
		}

		/*
		 * Commit and start the next trans in the chain.
		 */
		if ((error = xfs_attr_rolltrans(&args->trans, dp)))
			goto out;

	} else if (args->rmtblkno > 0) {
		/*
		 * Added a "remote" value, just clear the incomplete flag.
		 */
		error = xfs_attr_leaf_clearflag(args);
		if (error)
			goto out;
	}
	retval = error = 0;

out:
	if (state)
		xfs_da_state_free(state);
	if (error)
		return(error);
	return(retval);
}

/*
 * Remove a name from a B-tree attribute list.
 *
 * This will involve walking down the Btree, and may involve joining
 * leaf nodes and even joining intermediate nodes up to and including
 * the root node (a special case of an intermediate node).
 */
STATIC int
xfs_attr_node_removename(xfs_da_args_t *args)
{
	xfs_da_state_t *state;
	xfs_da_state_blk_t *blk;
	xfs_inode_t *dp;
	xfs_dabuf_t *bp;
	int retval, error, committed, forkoff;

	/*
	 * Tie a string around our finger to remind us where we are.
	 */
	dp = args->dp;
	state = xfs_da_state_alloc();
	state->args = args;
	state->mp = dp->i_mount;
	state->blocksize = state->mp->m_sb.sb_blocksize;
	state->node_ents = state->mp->m_attr_node_ents;

	/*
	 * Search to see if name exists, and get back a pointer to it.
	 */
	error = xfs_da_node_lookup_int(state, &retval);
	if (error || (retval != EEXIST)) {
		if (error == 0)
			error = retval;
		goto out;
	}

	/*
	 * If there is an out-of-line value, de-allocate the blocks.
	 * This is done before we remove the attribute so that we don't
	 * overflow the maximum size of a transaction and/or hit a deadlock.
	 */
	blk = &state->path.blk[ state->path.active-1 ];
	ASSERT(blk->bp != NULL);
	ASSERT(blk->magic == XFS_ATTR_LEAF_MAGIC);
	if (args->rmtblkno > 0) {
		/*
		 * Fill in disk block numbers in the state structure
		 * so that we can get the buffers back after we commit
		 * several transactions in the following calls.
		 */
		error = xfs_attr_fillstate(state);
		if (error)
			goto out;

		/*
		 * Mark the attribute as INCOMPLETE, then bunmapi() the
		 * remote value.
		 */
		error = xfs_attr_leaf_setflag(args);
		if (error)
			goto out;
		error = xfs_attr_rmtval_remove(args);
		if (error)
			goto out;

		/*
		 * Refill the state structure with buffers, the prior calls
		 * released our buffers.
		 */
		error = xfs_attr_refillstate(state);
		if (error)
			goto out;
	}

	/*
	 * Remove the name and update the hashvals in the tree.
	 */
	blk = &state->path.blk[ state->path.active-1 ];
	ASSERT(blk->magic == XFS_ATTR_LEAF_MAGIC);
	retval = xfs_attr_leaf_remove(blk->bp, args);
	xfs_da_fixhashpath(state, &state->path);

	/*
	 * Check to see if the tree needs to be collapsed.
	 */
	if (retval && (state->path.active > 1)) {
		XFS_BMAP_INIT(args->flist, args->firstblock);
		error = xfs_da_join(state);
		if (!error) {
			error = xfs_bmap_finish(&args->trans, args->flist,
						*args->firstblock, &committed);
		}
		if (error) {
			ASSERT(committed);
			args->trans = NULL;
			xfs_bmap_cancel(args->flist);
			goto out;
		}

		/*
		 * bmap_finish() may have committed the last trans and started
		 * a new one.  We need the inode to be in all transactions.
		 */
		if (committed) {
			xfs_trans_ijoin(args->trans, dp, XFS_ILOCK_EXCL);
			xfs_trans_ihold(args->trans, dp);
		}

		/*
		 * Commit the Btree join operation and start a new trans.
		 */
		if ((error = xfs_attr_rolltrans(&args->trans, dp)))
			goto out;
	}

	/*
	 * If the result is small enough, push it all into the inode.
	 */
	if (xfs_bmap_one_block(dp, XFS_ATTR_FORK)) {
		/*
		 * Have to get rid of the copy of this dabuf in the state.
		 */
		ASSERT(state->path.active == 1);
		ASSERT(state->path.blk[0].bp);
		xfs_da_buf_done(state->path.blk[0].bp);
		state->path.blk[0].bp = NULL;

		error = xfs_da_read_buf(args->trans, args->dp, 0, -1, &bp,
						     XFS_ATTR_FORK);
		if (error)
			goto out;
		ASSERT(INT_GET(((xfs_attr_leafblock_t *)
				      bp->data)->hdr.info.magic, ARCH_CONVERT)
						       == XFS_ATTR_LEAF_MAGIC);

		if ((forkoff = xfs_attr_shortform_allfit(bp, dp))) {
			XFS_BMAP_INIT(args->flist, args->firstblock);
			error = xfs_attr_leaf_to_shortform(bp, args, forkoff);
			/* bp is gone due to xfs_da_shrink_inode */
			if (!error) {
				error = xfs_bmap_finish(&args->trans,
							args->flist,
							*args->firstblock,
							&committed);
			}
			if (error) {
				ASSERT(committed);
				args->trans = NULL;
				xfs_bmap_cancel(args->flist);
				goto out;
			}

			/*
			 * bmap_finish() may have committed the last trans
			 * and started a new one.  We need the inode to be
			 * in all transactions.
			 */
			if (committed) {
				xfs_trans_ijoin(args->trans, dp, XFS_ILOCK_EXCL);
				xfs_trans_ihold(args->trans, dp);
			}
		} else
			xfs_da_brelse(args->trans, bp);
	}
	error = 0;

out:
	xfs_da_state_free(state);
	return(error);
}

/*
 * Fill in the disk block numbers in the state structure for the buffers
 * that are attached to the state structure.
 * This is done so that we can quickly reattach ourselves to those buffers
 * after some set of transaction commit's has released these buffers.
 */
STATIC int
xfs_attr_fillstate(xfs_da_state_t *state)
{
	xfs_da_state_path_t *path;
	xfs_da_state_blk_t *blk;
	int level;

	/*
	 * Roll down the "path" in the state structure, storing the on-disk
	 * block number for those buffers in the "path".
	 */
	path = &state->path;
	ASSERT((path->active >= 0) && (path->active < XFS_DA_NODE_MAXDEPTH));
	for (blk = path->blk, level = 0; level < path->active; blk++, level++) {
		if (blk->bp) {
			blk->disk_blkno = xfs_da_blkno(blk->bp);
			xfs_da_buf_done(blk->bp);
			blk->bp = NULL;
		} else {
			blk->disk_blkno = 0;
		}
	}

	/*
	 * Roll down the "altpath" in the state structure, storing the on-disk
	 * block number for those buffers in the "altpath".
	 */
	path = &state->altpath;
	ASSERT((path->active >= 0) && (path->active < XFS_DA_NODE_MAXDEPTH));
	for (blk = path->blk, level = 0; level < path->active; blk++, level++) {
		if (blk->bp) {
			blk->disk_blkno = xfs_da_blkno(blk->bp);
			xfs_da_buf_done(blk->bp);
			blk->bp = NULL;
		} else {
			blk->disk_blkno = 0;
		}
	}

	return(0);
}

/*
 * Reattach the buffers to the state structure based on the disk block
 * numbers stored in the state structure.
 * This is done after some set of transaction commit's has released those
 * buffers from our grip.
 */
STATIC int
xfs_attr_refillstate(xfs_da_state_t *state)
{
	xfs_da_state_path_t *path;
	xfs_da_state_blk_t *blk;
	int level, error;

	/*
	 * Roll down the "path" in the state structure, storing the on-disk
	 * block number for those buffers in the "path".
	 */
	path = &state->path;
	ASSERT((path->active >= 0) && (path->active < XFS_DA_NODE_MAXDEPTH));
	for (blk = path->blk, level = 0; level < path->active; blk++, level++) {
		if (blk->disk_blkno) {
			error = xfs_da_read_buf(state->args->trans,
						state->args->dp,
						blk->blkno, blk->disk_blkno,
						&blk->bp, XFS_ATTR_FORK);
			if (error)
				return(error);
		} else {
			blk->bp = NULL;
		}
	}

	/*
	 * Roll down the "altpath" in the state structure, storing the on-disk
	 * block number for those buffers in the "altpath".
	 */
	path = &state->altpath;
	ASSERT((path->active >= 0) && (path->active < XFS_DA_NODE_MAXDEPTH));
	for (blk = path->blk, level = 0; level < path->active; blk++, level++) {
		if (blk->disk_blkno) {
			error = xfs_da_read_buf(state->args->trans,
						state->args->dp,
						blk->blkno, blk->disk_blkno,
						&blk->bp, XFS_ATTR_FORK);
			if (error)
				return(error);
		} else {
			blk->bp = NULL;
		}
	}

	return(0);
}

/*
 * Write the value associated with an attribute into the out-of-line buffer
 * that we have defined for it.
 */
STATIC int
xfs_attr_rmtval_set(xfs_da_args_t *args)
{
	xfs_mount_t *mp;
	xfs_fileoff_t lfileoff;
	xfs_inode_t *dp;
	xfs_bmbt_irec_t map;
	xfs_daddr_t dblkno;
	xfs_caddr_t src;
	xfs_buf_t *bp;
	xfs_dablk_t lblkno;
	int blkcnt, valuelen, nmap, error, tmp, committed;

	dp = args->dp;
	mp = dp->i_mount;
	src = args->value;

	/*
	 * Find a "hole" in the attribute address space large enough for
	 * us to drop the new attribute's value into.
	 */
	blkcnt = XFS_B_TO_FSB(mp, args->valuelen);
	lfileoff = 0;
	error = xfs_bmap_first_unused(args->trans, args->dp, blkcnt, &lfileoff,
						   XFS_ATTR_FORK);
	if (error) {
		return(error);
	}
	args->rmtblkno = lblkno = (xfs_dablk_t)lfileoff;
	args->rmtblkcnt = blkcnt;

	/*
	 * Roll through the "value", allocating blocks on disk as required.
	 */
	while (blkcnt > 0) {
		/*
		 * Allocate a single extent, up to the size of the value.
		 */
		XFS_BMAP_INIT(args->flist, args->firstblock);
		nmap = 1;
		error = xfs_bmapi(args->trans, dp, (xfs_fileoff_t)lblkno,
				  blkcnt,
				  XFS_BMAPI_ATTRFORK | XFS_BMAPI_METADATA |
							XFS_BMAPI_WRITE,
				  args->firstblock, args->total, &map, &nmap,
				  args->flist);
		if (!error) {
			error = xfs_bmap_finish(&args->trans, args->flist,
						*args->firstblock, &committed);
		}
		if (error) {
			ASSERT(committed);
			args->trans = NULL;
			xfs_bmap_cancel(args->flist);
			return(error);
		}

		/*
		 * bmap_finish() may have committed the last trans and started
		 * a new one.  We need the inode to be in all transactions.
		 */
		if (committed) {
			xfs_trans_ijoin(args->trans, dp, XFS_ILOCK_EXCL);
			xfs_trans_ihold(args->trans, dp);
		}

		ASSERT(nmap == 1);
		ASSERT((map.br_startblock != DELAYSTARTBLOCK) &&
		       (map.br_startblock != HOLESTARTBLOCK));
		lblkno += map.br_blockcount;
		blkcnt -= map.br_blockcount;

		/*
		 * Start the next trans in the chain.
		 */
		if ((error = xfs_attr_rolltrans(&args->trans, dp)))
			return (error);
	}

	/*
	 * Roll through the "value", copying the attribute value to the
	 * already-allocated blocks.  Blocks are written synchronously
	 * so that we can know they are all on disk before we turn off
	 * the INCOMPLETE flag.
	 */
	lblkno = args->rmtblkno;
	valuelen = args->valuelen;
	while (valuelen > 0) {
		/*
		 * Try to remember where we decided to put the value.
		 */
		XFS_BMAP_INIT(args->flist, args->firstblock);
		nmap = 1;
		error = xfs_bmapi(NULL, dp, (xfs_fileoff_t)lblkno,
				  args->rmtblkcnt,
				  XFS_BMAPI_ATTRFORK | XFS_BMAPI_METADATA,
				  args->firstblock, 0, &map, &nmap, NULL);
		if (error) {
			return(error);
		}
		ASSERT(nmap == 1);
		ASSERT((map.br_startblock != DELAYSTARTBLOCK) &&
		       (map.br_startblock != HOLESTARTBLOCK));

		dblkno = XFS_FSB_TO_DADDR(mp, map.br_startblock),
		blkcnt = XFS_FSB_TO_BB(mp, map.br_blockcount);

		bp = xfs_buf_get_flags(mp->m_ddev_targp, dblkno,
							blkcnt, XFS_BUF_LOCK);
		ASSERT(bp);
		ASSERT(!XFS_BUF_GETERROR(bp));

		tmp = (valuelen < XFS_BUF_SIZE(bp)) ? valuelen :
							XFS_BUF_SIZE(bp);
		xfs_biomove(bp, 0, tmp, src, XFS_B_WRITE);
		if (tmp < XFS_BUF_SIZE(bp))
			xfs_biozero(bp, tmp, XFS_BUF_SIZE(bp) - tmp);
		if ((error = xfs_bwrite(mp, bp))) {/* GROT: NOTE: synchronous write */
			return (error);
		}
		src += tmp;
		valuelen -= tmp;

		lblkno += map.br_blockcount;
	}
	ASSERT(valuelen == 0);
	return(0);
}

/*
 * Remove the value associated with an attribute by deleting the
 * out-of-line buffer that it is stored on.
 */
STATIC int
xfs_attr_rmtval_remove(xfs_da_args_t *args)
{
	xfs_mount_t *mp;
	xfs_bmbt_irec_t map;
	xfs_buf_t *bp;
	xfs_daddr_t dblkno;
	xfs_dablk_t lblkno;
	int valuelen, blkcnt, nmap, error, done, committed;

	mp = args->dp->i_mount;

	/*
	 * Roll through the "value", invalidating the attribute value's
	 * blocks.
	 */
	lblkno = args->rmtblkno;
	valuelen = args->rmtblkcnt;
	while (valuelen > 0) {
		/*
		 * Try to remember where we decided to put the value.
		 */
		XFS_BMAP_INIT(args->flist, args->firstblock);
		nmap = 1;
		error = xfs_bmapi(NULL, args->dp, (xfs_fileoff_t)lblkno,
					args->rmtblkcnt,
					XFS_BMAPI_ATTRFORK | XFS_BMAPI_METADATA,
					args->firstblock, 0, &map, &nmap,
					args->flist);
		if (error) {
			return(error);
		}
		ASSERT(nmap == 1);
		ASSERT((map.br_startblock != DELAYSTARTBLOCK) &&
		       (map.br_startblock != HOLESTARTBLOCK));

		dblkno = XFS_FSB_TO_DADDR(mp, map.br_startblock),
		blkcnt = XFS_FSB_TO_BB(mp, map.br_blockcount);

		/*
		 * If the "remote" value is in the cache, remove it.
		 */
		bp = xfs_incore(mp->m_ddev_targp, dblkno, blkcnt,
				XFS_INCORE_TRYLOCK);
		if (bp) {
			XFS_BUF_STALE(bp);
			XFS_BUF_UNDELAYWRITE(bp);
			xfs_buf_relse(bp);
			bp = NULL;
		}

		valuelen -= map.br_blockcount;

		lblkno += map.br_blockcount;
	}

	/*
	 * Keep de-allocating extents until the remote-value region is gone.
	 */
	lblkno = args->rmtblkno;
	blkcnt = args->rmtblkcnt;
	done = 0;
	while (!done) {
		XFS_BMAP_INIT(args->flist, args->firstblock);
		error = xfs_bunmapi(args->trans, args->dp, lblkno, blkcnt,
				    XFS_BMAPI_ATTRFORK | XFS_BMAPI_METADATA,
				    1, args->firstblock, args->flist, &done);
		if (!error) {
			error = xfs_bmap_finish(&args->trans, args->flist,
						*args->firstblock, &committed);
		}
		if (error) {
			ASSERT(committed);
			args->trans = NULL;
			xfs_bmap_cancel(args->flist);
			return(error);
		}

		/*
		 * bmap_finish() may have committed the last trans and started
		 * a new one.  We need the inode to be in all transactions.
		 */
		if (committed) {
			xfs_trans_ijoin(args->trans, args->dp, XFS_ILOCK_EXCL);
			xfs_trans_ihold(args->trans, args->dp);
		}

		/*
		 * Close out trans and start the next one in the chain.
		 */
		if ((error = xfs_attr_rolltrans(&args->trans, args->dp)))
			return (error);
	}
	return(0);
}