/*
 * CDDL HEADER START
 *
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2016 by Delphix. All rights reserved.
 */

#include "lua.h"
#include "lualib.h"
#include "lauxlib.h"

#include <sys/dsl_prop.h>
#include <sys/dsl_dir.h>
#include <sys/dsl_synctask.h>
#include <sys/dsl_dataset.h>
#include <sys/zcp.h>
#include <sys/zcp_iter.h>
#include <sys/zcp_global.h>
#include <sys/zcp_set.h>
#include <sys/zvol.h>

#include <zfs_prop.h>

static int
zcp_set_user_prop(lua_State *state, dsl_pool_t *dp, const char *dsname,
    const char *prop_name, const char *prop_val, dmu_tx_t *tx)
{
	dsl_dataset_t *ds = zcp_dataset_hold(state, dp, dsname, FTAG);
	if (ds == NULL)
		return (1);/* not reached; zcp_dataset_hold() longjmp'd */

	nvlist_t *nvl = fnvlist_alloc();
	fnvlist_add_string(nvl, prop_name, prop_val);

	dsl_props_set_sync_impl(ds, ZPROP_SRC_LOCAL, nvl, tx);

	fnvlist_free(nvl);
	dsl_dataset_rele(ds, FTAG);
	return (0);
}

static int
parse_prop_value(zprop_type_t prop_type, nvlist_t *nvl, zfs_prop_t zfs_prop,
    const char *prop_val)
{
	int error = 0;
	uint64_t num_val;
	const char *prop_name = zfs_prop_to_name(zfs_prop);

	switch(prop_type) {
	case PROP_TYPE_NUMBER: {
		char *end;
#ifdef _KERNEL
		(void) ddi_strtoll(prop_val, &end, 10, (longlong_t *) &num_val);
#else
		num_val = strtoull(prop_val, &end, 10);
#endif
		if (*end != '\0') {
			error = EINVAL;
		} else {
			fnvlist_add_uint64(nvl, prop_name, num_val);
		}
		break;
	}
	case PROP_TYPE_STRING:
		fnvlist_add_string(nvl, prop_name, prop_val);
		break;
	case PROP_TYPE_INDEX:
		error = zfs_prop_string_to_index(zfs_prop, prop_val, &num_val);
		if (error == 0) {
			fnvlist_add_uint64(nvl, prop_name, num_val);
		}
		break;
	}
	return (error);
}

static int
zcp_set_special_prop_sync(lua_State *state, const char *dsname,
    zfs_prop_t zfs_prop, nvpair_t *pair, dmu_tx_t *tx)
{
	uint64_t num_val;
	const char *prop_name = zfs_prop_to_name(zfs_prop);
	zprop_source_t source = ZPROP_SRC_LOCAL;

	VERIFY(0 == nvpair_value_uint64(pair, &num_val));

	dsl_dir_set_qr_arg_t ddsqra;
	ddsqra.ddsqra_name = dsname;
	ddsqra.ddsqra_source = source;
	ddsqra.ddsqra_value = num_val;

	switch(zfs_prop) {
	case ZFS_PROP_QUOTA:
		dsl_dir_set_quota_sync(&ddsqra, tx);
		break;
	case ZFS_PROP_REFQUOTA:
		dsl_dataset_set_refquota_sync(&ddsqra, tx);
		break;
	case ZFS_PROP_FILESYSTEM_LIMIT:
	case ZFS_PROP_SNAPSHOT_LIMIT:
		dsl_dir_actv_fs_ss_limit_sync(&dsname, tx);
		break;
	case ZFS_PROP_RESERVATION:
		dsl_dir_set_reservation_sync(&ddsqra, tx);
		break;
	case ZFS_PROP_REFRESERVATION:
		dsl_dataset_set_refreservation_sync(&ddsqra, tx);
		break;
#ifdef _KERNEL
	case ZFS_PROP_VOLSIZE:
		zvol_set_volsize(dsname, num_val);
		break;
#endif
	default:
		return (ENOENT);
	}
	return (0);
}

static int
zcp_set_special_prop_check(const char *dsname, zfs_prop_t zfs_prop,
    nvpair_t *pair, dmu_tx_t *tx)
{
	int error = 0;
	uint64_t num_val;
	const char *prop_name = zfs_prop_to_name(zfs_prop);
	zprop_source_t source = ZPROP_SRC_LOCAL;

	error = nvpair_value_uint64(pair, &num_val);

	dsl_dir_set_qr_arg_t ddsqra;
	ddsqra.ddsqra_name = dsname;
	ddsqra.ddsqra_source = source;
	ddsqra.ddsqra_value = num_val;

	switch (zfs_prop) {
	case ZFS_PROP_QUOTA:
		if (error != 0)
			break;
		error = dsl_dir_set_quota_check(&ddsqra, tx);
		break;
	case ZFS_PROP_REFQUOTA:
		if (error != 0)
			break;
		error = dsl_dataset_set_refquota_check(&ddsqra, tx);
		break;
	case ZFS_PROP_FILESYSTEM_LIMIT:
	case ZFS_PROP_SNAPSHOT_LIMIT:
		if (error != 0)
			break;
		error = dsl_dir_actv_fs_ss_limit_check(&dsname, tx);
		break;
	case ZFS_PROP_RESERVATION:
		if (error != 0)
			break;
		error = dsl_dir_set_reservation_check(&ddsqra, tx);
		break;
	case ZFS_PROP_REFRESERVATION:
		if (error != 0)
			break;
		error = dsl_dataset_set_refreservation_check(&ddsqra, tx);
		break;
	/* TODO: ZFS_PROP_VERSION
	*  ZFS_PROP_VOLBLOCKSIZE
	*  ZFS_PROP_RECORDSIZE
	*  ZFS_PROP_MLSLABEL
	*  ZFS_PROP_MOUNTPOINT
	*  ZFS_PROP_SHARESMB
	*  ZFS_PROP_SHARENFS
	*  ZFS_PROP_UTF8ONLY
	*  ZFS_PROP_NORMALIZE
	*/
	default:
		return (ENOENT);
	}
	return (error);
}

static int
zcp_set_system_prop(lua_State *state, dsl_pool_t *dp, const char *dsname,
    zfs_prop_t zfs_prop, const char *prop_val, dmu_tx_t *tx)
{
	const char *prop_name = zfs_prop_to_name(zfs_prop);
	zprop_type_t prop_type = zfs_prop_get_type(zfs_prop);
	nvlist_t *nvl = fnvlist_alloc();
	parse_prop_value(prop_type, nvl, zfs_prop, prop_val);

	if (zcp_set_special_prop_sync(state, dsname, zfs_prop,
	    nvlist_next_nvpair(nvl, NULL), tx) != 0) {
		dsl_dataset_t *ds = zcp_dataset_hold(state, dp, dsname, FTAG);
		if (ds == NULL)
			return (1);/* not reached; zcp_dataset_hold() longjmp'd */
		dsl_props_set_sync_impl(ds, ZPROP_SRC_LOCAL, nvl, tx);
		dsl_dataset_rele(ds, FTAG);
	}
	fnvlist_free(nvl);
	return (0);
}

int
zcp_set_prop_check(void *arg, dmu_tx_t *tx)
{
	int error;
	uint64_t num_val;
	zcp_set_prop_arg_t *args = arg;
	zcp_run_info_t *ri = zcp_run_info(args->state);
	dsl_pool_t *dp = ri->zri_pool;

	const char *dsname = args->dsname;
	const char *prop_name = args->prop;
	const char *prop_val = args->val;

	/* TODO use zfs_valid_proplist check to verify props here */
	if (zfs_prop_user(prop_name)) {
		return (0);
	}

	if (zfs_prop_userquota(prop_name)) {
		/* TODO Can we set this? */
		zfs_dbgmsg("Failing userquota prop");
		return (EINVAL);
	}

	zfs_prop_t zfs_prop = zfs_name_to_prop(prop_name);
	/* Invalid property name */
	if (zfs_prop == ZPROP_INVAL) {
		zfs_dbgmsg("Invalid property name");
		return (EINVAL);
	}

	/* Trying to set a read only property */
	/* TODO and not zfs_prop_setonce(prop) */
	if (zfs_prop_readonly(zfs_prop)) {
		zfs_dbgmsg("Trying to set a readonly prop");
		return (EINVAL);
	}

	zprop_type_t prop_type = zfs_prop_get_type(zfs_prop);
	zfs_dbgmsg("Property type passed: %d", prop_type);
	zfs_dbgmsg("Property passed: %d", zfs_prop);
/*	if (!zfs_prop_valid_for_type(zfs_prop, prop_type)) {
		zfs_dbgmsg("Property passed value of invalid type");
		return (EINVAL);
	}
*/

	nvlist_t *nvl = fnvlist_alloc();
	error = parse_prop_value(prop_type, nvl, zfs_prop, prop_val);
	if (error != 0) {
		zfs_dbgmsg("Failed to parse value");
		fnvlist_free(nvl);
		return (error);
	}
	/* TODO should we be using zfs_valid_proplist to verify props here? */
	error = zcp_set_special_prop_check(dsname, zfs_prop,
	    nvlist_next_nvpair(nvl, NULL), tx);
	if (error == ENOENT) {
		dsl_props_set_arg_t dpsa;
		dpsa.dpsa_dsname = dsname;
		dpsa.dpsa_source = ZPROP_SRC_LOCAL;
		dpsa.dpsa_props = nvl;
		/* TODO should this check be applied to user props as well? */
		error = dsl_props_set_check(&dpsa, tx);
	}
	fnvlist_free(nvl);

	if (error != 0)
		zfs_dbgmsg("Failed set special prop check");
	return (error);
}

void
zcp_set_prop_sync(void *arg, dmu_tx_t *tx)
{
        zcp_set_prop_arg_t *args = arg;
	zcp_run_info_t *ri = zcp_run_info(args->state);
	dsl_pool_t *dp = ri->zri_pool;

	const char *dsname = args->dsname;
	const char *prop_name = args->prop;
	const char *prop_val = args->val;

	/* User defined property */
	if (zfs_prop_user(prop_name)) {
		(void) zcp_set_user_prop(args->state, dp, dsname, prop_name,
		    prop_val, tx);
	} else {
		zfs_prop_t zfs_prop = zfs_name_to_prop(prop_name);
		/* Valid system property */
		VERIFY(zfs_prop != ZPROP_INVAL);
		VERIFY(!zfs_prop_readonly(zfs_prop));
		(void) zcp_set_system_prop(args->state, dp, dsname, zfs_prop,
		    prop_val, tx);
	}
}
