/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2019 Joyent, Inc.
 */

#include <libnvpair.h>
#include <fm/topo_mod.h>

#include <sys/fm/protocol.h>
#include <sys/types.h>

#include <fcntl.h>
#include <unistd.h>
#include <topo_digraph.h>
#include <topo_sas.h>
#include <topo_method.h>
#include <topo_subr.h>
#include "sas.h"

#include <smhbaapi.h>
#include <scsi/libsmp.h>
#include <sys/libdevid.h>
#include <sys/time.h>

#include <sys/scsi/scsi.h>
#include <sys/scsi/generic/sas.h>

#pragma pack(1)

/*
 * Mode Parameter Header
 *
 * See SPC 4, sectin 7.5.5
 */
typedef struct scsi_mode10_hdr {
	uint16_t smh10_len;
	uint8_t smh10_medtype;
	uint8_t smh10_devparm;
	uint16_t _reserved;
	uint16_t smh10_bdlen;
} scsi_mode10_hdr_t;

/*
 * SCSI MODE SENSE (10) command
 *
 * See SPC 4, section 6.14
 */
typedef struct scsi_modesense_10_cmd {
	uint8_t scm10_opcode;
	DECL_BITFIELD4(
		_reserved1	:3,
		scm10_dbd	:1,
		scm10_llbaa	:1,
		_reserved2	:3);
	DECL_BITFIELD2(
		scm10_pagecode	:6,
		scm10_pc	:2);
	uint8_t scm10_subpagecode;
	uint8_t _reserved3[3];
	uint16_t scm10_buflen;
	uint8_t scm10_control;
} scsi_modesense_10_cmd_t;

#pragma pack()

static int
scsi_mode_sense(topo_mod_t *mod, int fd, uint8_t pagecode, uint8_t subpagecode,
    uchar_t *pagebuf, uint16_t buflen)
{
	struct uscsi_cmd ucmd_buf;
	scsi_modesense_10_cmd_t cdb;
	struct scsi_extended_sense sense_buf;

	memset((void *)pagebuf, 0, buflen);
	memset((void *)&cdb, 0, sizeof (scsi_modesense_10_cmd_t));
	memset((void *)&ucmd_buf, 0, sizeof (ucmd_buf));
	memset((void *)&sense_buf, 0, sizeof (sense_buf));

	cdb.scm10_opcode = SCMD_MODE_SENSE_G1;
	cdb.scm10_pagecode = pagecode;
	cdb.scm10_subpagecode = subpagecode;
	cdb.scm10_buflen = BE_16(buflen);

	ucmd_buf.uscsi_cdb = (char *)&cdb;
	ucmd_buf.uscsi_cdblen = sizeof (scsi_modesense_10_cmd_t);
	ucmd_buf.uscsi_bufaddr = (caddr_t)pagebuf;
	ucmd_buf.uscsi_buflen = buflen;
	ucmd_buf.uscsi_rqbuf = (caddr_t)&sense_buf;
	ucmd_buf.uscsi_rqlen = sizeof (struct scsi_extended_sense);
	ucmd_buf.uscsi_flags = USCSI_RQENABLE | USCSI_READ | USCSI_SILENT;
	ucmd_buf.uscsi_timeout = 60;

	if (ioctl(fd, USCSICMD, &ucmd_buf) < 0) {
		topo_mod_dprintf(mod, "failed to read mode page (%s)",
		    strerror(errno));
		return (topo_mod_seterrno(mod, EMOD_UNKNOWN));
	}
	return (0);
}

static int
scsi_log_sense(topo_mod_t *mod, int fd, uint8_t pagecode, uchar_t *pagebuf,
    uint16_t pagelen)
{
	struct uscsi_cmd ucmd_buf;
	uchar_t	cdb_buf[CDB_GROUP1];
	struct scsi_extended_sense sense_buf;

	memset((void *)pagebuf, 0, pagelen);
	memset((void *)&cdb_buf, 0, sizeof (cdb_buf));
	memset((void *)&ucmd_buf, 0, sizeof (ucmd_buf));
	memset((void *)&sense_buf, 0, sizeof (sense_buf));

	cdb_buf[0] = SCMD_LOG_SENSE_G1;
	cdb_buf[2] = (0x01 << 6) | pagecode;
	cdb_buf[7] = (uchar_t)((pagelen & 0xFF00) >> 8);
	cdb_buf[8] = (uchar_t)(pagelen  & 0x00FF);

	ucmd_buf.uscsi_cdb = (char *)cdb_buf;
	ucmd_buf.uscsi_cdblen = sizeof (cdb_buf);
	ucmd_buf.uscsi_bufaddr = (caddr_t)pagebuf;
	ucmd_buf.uscsi_buflen = pagelen;
	ucmd_buf.uscsi_rqbuf = (caddr_t)&sense_buf;
	ucmd_buf.uscsi_rqlen = sizeof (struct scsi_extended_sense);
	ucmd_buf.uscsi_flags = USCSI_RQENABLE | USCSI_READ | USCSI_SILENT;
	ucmd_buf.uscsi_timeout = 60;

	if (ioctl(fd, USCSICMD, &ucmd_buf) < 0) {
		topo_mod_dprintf(mod, "failed to read log page (%s)",
		    strerror(errno));
		return (topo_mod_seterrno(mod, EMOD_UNKNOWN));
	}
	return (0);
}

/*
 * This is a prop method that returns the dev-scheme FMRI of an initiator or
 * target node.
 */
int
sas_dev_fmri(topo_mod_t *mod, tnode_t *node, topo_version_t version,
    nvlist_t *in, nvlist_t **out)
{
	char *devid = NULL, *devfs_path = NULL, *fmristr = NULL;
	const char *pg, *prop_devfs, *prop_devid, *prop_fmri;
	const char *nodename = topo_node_name(node);
	nvlist_t *fmri = NULL, *nvl = NULL, *pargs;
	int err, ret = -1;

	if (version > TOPO_METH_FMRI_VERSION)
		return (topo_mod_seterrno(mod, EMOD_VER_NEW));

	/*
	 * Look for a private argument list to determine if the invoker is
	 * trying to do a set operation and if so, return an error as this
	 * method only supports get operations.
	 */
	if ((nvlist_lookup_nvlist(in, TOPO_PROP_PARGS, &pargs) == 0) &&
	    nvlist_exists(pargs, TOPO_PROP_VAL_VAL)) {
		topo_mod_dprintf(mod, "%s: set operation not suppported",
		    __func__);
		return (topo_mod_seterrno(mod, EMOD_NVL_INVAL));
	}

	if (strcmp(nodename, TOPO_VTX_INITIATOR) == 0) {
		pg = TOPO_PGROUP_INITIATOR;
		prop_devfs = TOPO_PROP_INITIATOR_DEVFS_PATH;
		prop_devid = NULL;
		prop_fmri = TOPO_PROP_INITIATOR_DEV_FMRI;
	} else if (strcmp(nodename, TOPO_VTX_TARGET) == 0) {
		pg = TOPO_PGROUP_TARGET;
		prop_devfs = TOPO_PROP_TARGET_DEVFS_PATH;
		prop_devid = TOPO_PROP_TARGET_DEVID;
		prop_fmri = TOPO_PROP_TARGET_DEV_FMRI;
	}

	if (topo_prop_get_string(node, pg, prop_devfs, &devfs_path, &err) !=
	    0 || (prop_devid != NULL && topo_prop_get_string(node, pg,
	    prop_devid, &devid, &err) != 0)) {
		topo_mod_dprintf(mod, "%s: prop lookup failed (%s)", __func__,
		    topo_strerror(err));
		goto err;
	}
	if ((fmri = topo_mod_devfmri(mod, FM_DEV_SCHEME_VERSION, devfs_path,
	    devid)) == NULL) {
		topo_mod_dprintf(mod, "%s: failed to construct dev fmri",
		    __func__);
		/* errno set */
		goto err;
	}
	if (topo_mod_nvl2str(mod, fmri, &fmristr) != 0) {
		topo_mod_dprintf(mod, "%s: failed to convert fmri to string",
		    __func__);
		/* errno set */
		goto err;
	}
	if (topo_mod_nvalloc(mod, &nvl, NV_UNIQUE_NAME) != 0 ||
	    nvlist_add_string(nvl, TOPO_PROP_VAL_NAME, prop_fmri) != 0 ||
	    nvlist_add_uint32(nvl, TOPO_PROP_VAL_TYPE, TOPO_TYPE_STRING)
	    != 0 ||
	    nvlist_add_string(nvl, TOPO_PROP_VAL_VAL, fmristr) != 0) {

		topo_mod_dprintf(mod, "%s: Failed to allocate 'out' nvlist",
		    __func__);
		nvlist_free(nvl);
		ret = topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}
	*out = nvl;
	ret = 0;

err:
	topo_mod_strfree(mod, devid);
	topo_mod_strfree(mod, devfs_path);
	topo_mod_strfree(mod, fmristr);
	nvlist_free(fmri);
	return (ret);
}

/*
 * This routine is used to determine if the specified SAS topo node and HC
 * topo node represent the same physical device.
 */
static boolean_t
sas_node_matches_logical_disk(topo_mod_t *mod, tnode_t *sas_node,
    tnode_t *hc_node)
{
	char *ldisk = NULL, *orig_ldisk = NULL;
	uint_t ldisksz;
	uint64_t wwn;
	int err = 0;

	if (topo_prop_get_string(hc_node, TOPO_PGROUP_STORAGE,
	    TOPO_STORAGE_LOGICAL_DISK, &ldisk, &err) != 0) {
		topo_mod_dprintf(mod,
		    "failed to get logical-disk name (%s)", topo_strerror(err));
		goto done;
	}

	topo_mod_dprintf(mod, "target port ID not available, "
	    "attempting logical-disk name match on %" PRIx64 "=%s",
	    topo_node_instance(sas_node), ldisk);

	/*
	 * We get a logical-disk name that looks like this:
	 *   c0tWWNd0
	 *
	 * We want to compare the middle WWN part to the sas
	 * node's WWN. Once we pull the middle bit out we
	 * convert it to a uint64 so comparison is easier.
	 */
	ldisksz = strlen(ldisk) + 1;
	orig_ldisk = ldisk;

	ldisk = strchr(ldisk, 't');
	ldisk++;
	ldisk = strtok(ldisk, "d");

	/*
	 * This call fails for non-SAS devices that make it
	 * through our previous checks.
	 */
	if (scsi_wwnstr_to_wwn(ldisk, &wwn) != 0) {
		topo_mod_dprintf(mod,
		    "scsi_wwnstr_to_wwn failed for %s", ldisk);
		goto done;
	}

	topo_mod_free(mod, orig_ldisk, ldisksz);
	return (wwn == topo_node_instance(sas_node));
done:
	if (orig_ldisk != NULL)
		topo_mod_free(mod, orig_ldisk, ldisksz);
	return (B_FALSE);
}

/*
 * Find all ports attached to the given sas_node and see if their WWN matches
 * the given wwn.
 *
 * We do this because SAS targets and SAS target ports can have different WWNs.
 * The target port's WWN is stored in the HC tree.
 */
static boolean_t
sas_node_matches_l0id(topo_mod_t *mod, tnode_t *sas_node, uint64_t wwn)
{
	topo_edge_t *edge = NULL;
	for (edge = topo_list_next(&sas_node->tn_vtx->tvt_incoming);
	    edge != NULL; edge = topo_list_next(edge)) {
		topo_vertex_t *vtx = edge->tve_vertex;

		if (wwn == topo_node_instance(topo_vertex_node(vtx))) {
			return (B_TRUE);
		}
	}

	return (B_FALSE);
}

typedef struct sas_topo_cbarg {
	topo_mod_t *st_mod;
	tnode_t *st_node;
	void *st_ret;
} sas_topo_cbarg_t;

/*
 * Called for every node in the hc tree. This function determines if the given
 * hc node corresponds to the sas node in arg->st_node. If the two nodes refer
 * to the same device then we retrieve the resource string and copy it into
 * the sas node's property group.
 *
 * The hc resource is later used to associate other hc properties, like serial
 * number, device manufacturer, etc. with sas nodes.
 */
static int
hc_iter_cb(topo_hdl_t *thp, tnode_t *node, void *arg)
{
	sas_topo_cbarg_t *cbarg = (sas_topo_cbarg_t *)arg;
	tnode_t *sas_node = cbarg->st_node;
	int err = 0;
	nvlist_t *fmri = NULL, *rsrc = NULL;
	char *fmristr = NULL;
	tnode_t *targ_node = NULL;
	topo_mod_t *mod = cbarg->st_mod;

	if (strcmp(topo_node_name(node), PCIEX_FUNCTION) == 0 &&
	    strcmp(topo_node_name(sas_node), TOPO_VTX_INITIATOR) == 0) {
		char *sas_devfsn = NULL;
		char *hc_devfsn = NULL;
		char *sas_devfsn_short;

		if (topo_prop_get_string(sas_node, TOPO_PGROUP_INITIATOR,
		    TOPO_PROP_INITIATOR_DEVFS_PATH, &sas_devfsn, &err) != 0) {
			topo_mod_dprintf(mod, "%s: failed to lookup %s "
			    "property (%s)", __func__,
			    TOPO_PGROUP_INITIATOR, topo_strerror(err));
			(void) topo_mod_seterrno(mod, err);
			goto done;
		}

		if (topo_prop_get_fmri(node, TOPO_PGROUP_IO,
		    TOPO_IO_MODULE, &fmri, &err) != 0 ||
		    topo_prop_get_string(node, TOPO_PGROUP_IO,
		    TOPO_IO_DEV, &hc_devfsn, &err) != 0) {
			topo_mod_dprintf(mod, "%s: failed to get IO props"
			    " (%s)", __func__, topo_strerror(err));
			(void) topo_mod_seterrno(mod, err);
			topo_hdl_strfree(thp, sas_devfsn);
			goto done;
		}

		/*
		 * Match sas and hc topo nodes based on the discovered
		 * OS device names.
		 *
		 * The libsmhbaapi reported device name includes the leading
		 * '/devices' string. The hc device name doesn't include this,
		 * so we advance the pointer a bit to make the comparison.
		 */
		sas_devfsn_short = sas_devfsn + strlen("/devices");
		if (strcmp(sas_devfsn_short, hc_devfsn) != 0) {
			topo_hdl_strfree(thp, sas_devfsn);
			topo_hdl_strfree(thp, hc_devfsn);
			goto done;
		}
		topo_hdl_strfree(thp, sas_devfsn);
		topo_hdl_strfree(thp, hc_devfsn);

		/*
		 * We expect initiators to be using the mpt_sas driver.
		 * This won't work for non-mpt_sas topologies, but
		 * by this point those topos have already been
		 * ignored.
		 */
		if ((err = nvlist_lookup_string(fmri, FM_FMRI_MOD_NAME,
		    &fmristr)) != 0 ||
		    (err = strcmp(fmristr, "mpt_sas") != 0)) {
			(void) topo_mod_seterrno(mod, err);
			goto done;
		}

		targ_node = node;

	} else if (strcmp(topo_node_name(node), DISK) == 0 &&
	    strcmp(topo_node_name(sas_node), TOPO_VTX_TARGET) == 0) {
		uint_t nelem;
		uint64_t wwn;
		char **ids;

		/*
		 * Target port l0ids will be available when SES enumeration
		 * works properly. During SAS device discovery using SMP we
		 * retrieve a couple different WWNs for each device:
		 * - the target port WWN (the 'port' node's WWN)
		 * - the target device WWN (the 'target' node's WWN)
		 *
		 * SES will have the match for the target port WWN. If we cannot
		 * find a target port WWN that means we don't have SES and we
		 * should attempt to instead match against this node's WWN
		 * against the 'logical-disk' name.
		 */
		if (topo_prop_get_string_array(node, TOPO_PGROUP_STORAGE,
		    TOPO_STORAGE_TARGET_PORT_L0IDS, &ids, &nelem, &err) != 0) {
			if (err != ETOPO_PROP_NOENT) {
				/*
				 * This prop will not be present in systems
				 * without SES.
				 */
				goto done;
			}
			if (sas_node_matches_logical_disk(
			    mod, sas_node, node)) {
				targ_node = node;
			}
		} else {
			/*
			 * Go through all of the ports attached to this target
			 * device. If any ports match any of the l0ids, then we
			 * have found an HC scheme match for this SAS scheme
			 * target.
			 */
			for (uint_t i = 0; i < nelem; i++) {
				if (scsi_wwnstr_to_wwn(ids[i], &wwn) != 0) {
					topo_mod_dprintf(mod,
					    "scsi_wwnstr_to_wwn failed for %s",
					    ids[i]);
					topo_hdl_strfreev(thp, ids, nelem);
					goto done;
				}
				if (sas_node_matches_l0id(mod, sas_node, wwn)) {
					targ_node = node;
				}
			}
			topo_hdl_strfreev(thp, ids, nelem);
		}
	}

	if (targ_node == NULL) {
		goto done;
	}

	if (topo_node_resource(targ_node, &rsrc, &err) != 0 ||
	    topo_fmri_nvl2str(thp, rsrc, &fmristr, &err) != 0) {
		topo_mod_dprintf(mod, "failed to get"
		    "resource string (%s)", topo_strerror(err));
		goto done;
	}
	cbarg->st_ret = fmristr;
	nvlist_free(fmri);
	nvlist_free(rsrc);
	return (TOPO_WALK_TERMINATE);

done:
	nvlist_free(fmri);
	return (TOPO_WALK_NEXT);
}

/*
 * This is a prop method that returns the hc-scheme FMRI of the corresponding
 * component in the hc-scheme topology.  This should be registered on the
 * underlying nodes for initiator and non-SMP target vertices.
 *
 * For initiators this would be the corresponding pciexfn node.
 * For disk/ssd targets, this would be the corresponding disk node.  For SES
 * targets, this would be the corresponding ses-enclosure node.  SMP targets
 * are not represented in the hc-scheme topology.
 */
int
sas_hc_fmri(topo_mod_t *mod, tnode_t *node, topo_version_t version,
    nvlist_t *in, nvlist_t **out)
{
	if (version > TOPO_METH_FMRI_VERSION)
		return (topo_mod_seterrno(mod, EMOD_VER_NEW));

	topo_hdl_t *thp = mod->tm_hdl;
	topo_walk_t *twp = NULL;
	sas_topo_cbarg_t cbarg = {
		.st_mod = mod,
		.st_node = node,
		.st_ret = NULL
	};
	int err, ret = -1;
	nvlist_t *result = NULL;
	const char *pname = NULL;

	if ((twp = topo_walk_init(thp, "hc", hc_iter_cb, &cbarg, &err))
	    == NULL) {
		topo_mod_dprintf(mod, "failed to init topo walker: %s",
		    topo_strerror(err));
		goto out;
	}
	if (topo_walk_step(twp, TOPO_WALK_CHILD) == TOPO_WALK_ERR) {
		topo_mod_dprintf(mod, "topo walker error");
		topo_walk_fini(twp);
		goto out;
	}
	topo_walk_fini(twp);

	if (cbarg.st_ret == NULL) {
		goto out;
	}

	if ((err = topo_mod_nvalloc(mod, &result, NV_UNIQUE_NAME)) != 0) {
		ret = topo_mod_seterrno(mod, err);
		goto out;
	}
	if (strcmp(topo_node_name(node), TOPO_VTX_INITIATOR) == 0) {
		pname = TOPO_PROP_INITIATOR_FMRI;
	} else {
		pname = TOPO_PROP_TARGET_HC_FMRI;
	}

	fnvlist_add_string(result, TOPO_PROP_VAL_NAME, pname);
	fnvlist_add_uint32(result, TOPO_PROP_VAL_TYPE, TOPO_TYPE_STRING);
	fnvlist_add_string(result, TOPO_PROP_VAL_VAL, strdup(cbarg.st_ret));
	*out = result;
	ret = 0;

out:
	topo_mod_strfree(mod, cbarg.st_ret);
	return (ret);
}

/*
 * This is the entrypoint for gathering stats from other modules.
 *
 * This will first look up the hc scheme fmri. The hc fmri is then used to look
 * up various other properties that are relevant to the sas devices.
 */
int
sas_device_props_set(topo_mod_t *mod, tnode_t *node, topo_version_t version,
    nvlist_t *in, nvlist_t **out)
{
	topo_hdl_t *thp = mod->tm_hdl;
	const char *pgroup = NULL;
	char *pname = NULL;
	const char *fmri_pname = NULL;
	char *val = NULL;
	nvlist_t *nvl = NULL;
	nvlist_t *pnvl = NULL;
	const char *targ_group = NULL;
	const char *targ_prop = NULL;
	int err, ret = -1;

	if (version > TOPO_METH_FMRI_VERSION)
		return (topo_mod_seterrno(mod, EMOD_VER_NEW));

	if (nvlist_lookup_nvlist(in, TOPO_PROP_ARGS, &nvl) != 0 ||
	    nvlist_lookup_string(nvl, "pname", &pname) != 0) {
		topo_mod_dprintf(mod, "%s: missing pname arg", __func__);
		return (topo_mod_seterrno(mod, EMOD_NVL_INVAL));
	}

	if (strcmp(topo_node_name(node), TOPO_VTX_INITIATOR) == 0) {
		pgroup = TOPO_PGROUP_INITIATOR;
		fmri_pname = TOPO_PROP_INITIATOR_FMRI;
		if (strcmp(pname, TOPO_PROP_INITIATOR_MANUF) == 0) {
			targ_group = TOPO_PGROUP_PCI;
			targ_prop = TOPO_PCI_VENDNM;
		} else if (strcmp(pname, TOPO_PROP_INITIATOR_MODEL) == 0) {
			targ_group = TOPO_PGROUP_PCI;
			targ_prop = TOPO_PCI_DEVNM;
		} else if (strcmp(pname, TOPO_PROP_INITIATOR_LABEL) == 0) {
			targ_group = TOPO_PGROUP_PROTOCOL;
			targ_prop = TOPO_PROP_LABEL;
		}
	} else if (strcmp(topo_node_name(node), TOPO_VTX_TARGET) == 0) {
		pgroup = TOPO_PGROUP_TARGET;
		fmri_pname = TOPO_PROP_TARGET_HC_FMRI;
		if (strcmp(pname, TOPO_PROP_TARGET_DEVFS_PATH) == 0) {
			targ_group = TOPO_PGROUP_IO;
			targ_prop = TOPO_IO_DEV_PATH;
		} else if (strcmp(pname, TOPO_PROP_TARGET_DEVID) == 0) {
			targ_group = TOPO_PGROUP_IO;
			targ_prop = TOPO_IO_DEVID;
		} else if (strcmp(pname, TOPO_PROP_TARGET_LABEL) == 0) {
			targ_group = TOPO_PGROUP_PROTOCOL;
			targ_prop = TOPO_PROP_LABEL;
		} else if (strcmp(pname, TOPO_PROP_TARGET_LOGICAL_DISK) == 0) {
			targ_group = TOPO_PGROUP_STORAGE;
			targ_prop = TOPO_STORAGE_LOGICAL_DISK;
		} else if (strcmp(pname, TOPO_PROP_TARGET_MANUF) == 0) {
			targ_group = TOPO_PGROUP_STORAGE;
			targ_prop = TOPO_STORAGE_MANUFACTURER;
		} else if (strcmp(pname, TOPO_PROP_TARGET_MODEL) == 0) {
			targ_group = TOPO_PGROUP_STORAGE;
			targ_prop = TOPO_STORAGE_MODEL;
		} else if (strcmp(pname, TOPO_PROP_TARGET_SERIAL) == 0) {
			targ_group = TOPO_PGROUP_STORAGE;
			targ_prop = TOPO_STORAGE_SERIAL_NUMBER;
		}
	}

	if (topo_prop_get_string(node, pgroup, fmri_pname, &val, &err) != 0) {
		topo_mod_dprintf(mod, "failed to get fmri for %s=%" PRIx64
		    " (%s)", topo_node_name(node), topo_node_instance(node),
		    topo_strerror(err));
		ret = topo_mod_seterrno(mod, err);
		goto done;
	}

	if (topo_fmri_str2nvl(thp, val, &nvl, &err) != 0) {
		topo_mod_dprintf(mod, "fmri_str2nvl failed for %s=%" PRIx64
		    " (%s)", topo_node_name(node), topo_node_instance(node),
		    topo_strerror(err));
		ret = topo_mod_seterrno(mod, err);
		goto done;
	}

	if (topo_fmri_getprop(thp, nvl, targ_group, targ_prop, NULL,
	    &pnvl, &err) != 0) {
		topo_mod_dprintf(mod, "getprop failed for %s=%" PRIx64
		    " (%s)",
		    topo_node_name(node), topo_node_instance(node),
		    topo_strerror(err));
		nvlist_free(pnvl);
		ret = topo_mod_seterrno(mod, err);
		goto done;
	}

	/*
	 * We re-use the nvlist that we got back from topo_fmri_getprop since
	 * it already has the value and type information we're looking for.
	 */
	if (nvlist_add_string(pnvl, TOPO_PROP_VAL_NAME, pname) != 0) {
		nvlist_free(pnvl);
		ret = topo_mod_seterrno(mod, EMOD_NOMEM);
		goto done;
	}
	*out = pnvl;

	ret = 0;
done:
	topo_mod_strfree(mod, val);
	nvlist_free(nvl);
	return (ret);
}

static ssize_t
fmri_bufsz(nvlist_t *nvl)
{
	nvlist_t **paths, *auth;
	uint_t nelem;
	char *type;
	ssize_t bufsz = 0;
	uint32_t start_phy = UINT32_MAX, end_phy = UINT32_MAX;

	if (nvlist_lookup_nvlist(nvl, FM_FMRI_AUTHORITY, &auth) != 0 ||
	    nvlist_lookup_string(auth, FM_FMRI_SAS_TYPE, &type) != 0)
		return (0);

	(void) nvlist_lookup_uint32(auth, FM_FMRI_SAS_START_PHY, &start_phy);
	(void) nvlist_lookup_uint32(auth, FM_FMRI_SAS_END_PHY, &end_phy);

	if (start_phy != UINT32_MAX && end_phy != UINT32_MAX) {
		bufsz += snprintf(NULL, 0, "sas://%s=%s:%s=%u:%s=%u",
		    FM_FMRI_SAS_TYPE, type, FM_FMRI_SAS_START_PHY, start_phy,
		    FM_FMRI_SAS_END_PHY, end_phy);
	} else {
		bufsz += snprintf(NULL, 0, "sas://%s=%s", FM_FMRI_SAS_TYPE,
		    type);
	}

	if (nvlist_lookup_nvlist_array(nvl, FM_FMRI_SAS_PATH, &paths,
	    &nelem) != 0) {
		return (0);
	}

	for (uint_t i = 0; i < nelem; i++) {
		char *sasname;
		uint64_t sasaddr;

		if (nvlist_lookup_string(paths[i], FM_FMRI_SAS_NAME,
		    &sasname) != 0 ||
		    nvlist_lookup_uint64(paths[i], FM_FMRI_SAS_ADDR,
		    &sasaddr) != 0) {
			return (0);
		}
		bufsz += snprintf(NULL, 0, "/%s=%" PRIx64 "", sasname,
		    sasaddr);
	}
	return (bufsz + 1);
}

int
sas_fmri_nvl2str(topo_mod_t *mod, tnode_t *node, topo_version_t version,
    nvlist_t *in, nvlist_t **out)
{
	uint8_t scheme_vers;
	nvlist_t *outnvl;
	nvlist_t **paths, *auth;
	uint_t nelem;
	ssize_t bufsz, end = 0;
	char *buf, *type;
	uint32_t start_phy = UINT32_MAX, end_phy = UINT32_MAX;

	if (version > TOPO_METH_NVL2STR_VERSION)
		return (topo_mod_seterrno(mod, EMOD_VER_NEW));

	if (nvlist_lookup_uint8(in, FM_FMRI_SAS_VERSION, &scheme_vers) != 0 ||
	    scheme_vers != FM_SAS_SCHEME_VERSION) {
		return (topo_mod_seterrno(mod, EMOD_FMRI_NVL));
	}

	/*
	 * Get size of buffer needed to hold the string representation of the
	 * FMRI.
	 */
	if ((bufsz = fmri_bufsz(in)) == 0) {
		return (topo_mod_seterrno(mod, EMOD_FMRI_MALFORM));
	}

	if ((buf = topo_mod_zalloc(mod, bufsz)) == NULL) {
		return (topo_mod_seterrno(mod, EMOD_NOMEM));
	}

	/*
	 * We've already successfully done these nvlist lookups in fmri_bufsz()
	 * so we don't worry about checking retvals this time around.
	 */
	(void) nvlist_lookup_nvlist(in, FM_FMRI_AUTHORITY, &auth);
	(void) nvlist_lookup_string(auth, FM_FMRI_SAS_TYPE, &type);
	(void) nvlist_lookup_uint32(auth, FM_FMRI_SAS_START_PHY, &start_phy);
	(void) nvlist_lookup_uint32(auth, FM_FMRI_SAS_END_PHY, &end_phy);
	(void) nvlist_lookup_nvlist_array(in, FM_FMRI_SAS_PATH, &paths,
	    &nelem);
	if (start_phy != UINT32_MAX && end_phy != UINT32_MAX)
		end += snprintf(buf, bufsz, "sas://%s=%s:%s=%u:%s=%u",
		    FM_FMRI_SAS_TYPE, type, FM_FMRI_SAS_START_PHY, start_phy,
		    FM_FMRI_SAS_END_PHY, end_phy);
	else
		end += snprintf(buf, bufsz, "sas://%s=%s", FM_FMRI_SAS_TYPE,
		    type);

	for (uint_t i = 0; i < nelem; i++) {
		char *sasname;
		uint64_t sasaddr;

		(void) nvlist_lookup_string(paths[i], FM_FMRI_SAS_NAME,
		    &sasname);
		(void) nvlist_lookup_uint64(paths[i], FM_FMRI_SAS_ADDR,
		    &sasaddr);
		end += snprintf(buf + end, (bufsz - end), "/%s=%" PRIx64 "",
		    sasname, sasaddr);
	}

	if (topo_mod_nvalloc(mod, &outnvl, NV_UNIQUE_NAME) != 0) {
		topo_mod_free(mod, buf, bufsz);
		return (topo_mod_seterrno(mod, EMOD_FMRI_NVL));
	}
	if (nvlist_add_string(outnvl, "fmri-string", buf) != 0) {
		nvlist_free(outnvl);
		topo_mod_free(mod, buf, bufsz);
		return (topo_mod_seterrno(mod, EMOD_FMRI_NVL));
	}
	topo_mod_free(mod, buf, bufsz);
	*out = outnvl;

	return (0);
}

int
sas_fmri_str2nvl(topo_mod_t *mod, tnode_t *node, topo_version_t version,
    nvlist_t *in, nvlist_t **out)
{
	char *fmristr, *tmp = NULL, *lastpair;
	char *sasname, *auth_field, *path_start;
	nvlist_t *fmri = NULL, *auth = NULL, **sas_path = NULL;
	uint_t npairs = 0, i = 0, fmrilen, path_offset;

	if (version > TOPO_METH_STR2NVL_VERSION)
		return (topo_mod_seterrno(mod, EMOD_VER_NEW));

	if (nvlist_lookup_string(in, "fmri-string", &fmristr) != 0)
		return (topo_mod_seterrno(mod, EMOD_METHOD_INVAL));

	if (strncmp(fmristr, "sas://", 6) != 0)
		return (topo_mod_seterrno(mod, EMOD_FMRI_MALFORM));

	if (topo_mod_nvalloc(mod, &fmri, NV_UNIQUE_NAME) != 0) {
		/* errno set */
		return (-1);
	}
	if (nvlist_add_string(fmri, FM_FMRI_SCHEME,
	    FM_FMRI_SCHEME_SAS) != 0 ||
	    nvlist_add_uint8(fmri, FM_FMRI_SAS_VERSION,
	    FM_SAS_SCHEME_VERSION) != 0) {
		(void) topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}

	/*
	 * We need to make a copy of the fmri string because strtok will
	 * modify it.  We can't use topo_mod_strdup/strfree because
	 * topo_mod_strfree will end up leaking part of the string because
	 * of the NUL chars that strtok inserts - which will cause
	 * topo_mod_strfree to miscalculate the length of the string.  So we
	 * keep track of the length of the original string and use
	 * topo_mod_zalloc/topo_mod_free.
	 */
	fmrilen = strlen(fmristr);
	if ((tmp = topo_mod_zalloc(mod, fmrilen + 1)) == NULL) {
		(void) topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}
	(void) strncpy(tmp, fmristr, fmrilen);

	/*
	 * Find the offset of the "/" after the authority portion of the FMRI.
	 */
	if ((path_start = strchr(tmp + 6, '/')) == NULL) {
		(void) topo_mod_seterrno(mod, EMOD_FMRI_MALFORM);
		topo_mod_free(mod, tmp, fmrilen + 1);
		goto err;
	}
	path_offset = path_start - tmp;

	/*
	 * Count the number of "=" chars after the "sas:///" portion of the
	 * FMRI to determine how big the sas-path array needs to be.
	 */
	(void) strtok_r(tmp + path_offset, "=", &lastpair);
	while (strtok_r(NULL, "=", &lastpair) != NULL)
		npairs++;

	if ((sas_path = topo_mod_zalloc(mod, npairs * sizeof (nvlist_t *))) ==
	    NULL) {
		(void) topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}

	/*
	 * Build the auth nvlist
	 */
	if (topo_mod_nvalloc(mod, &auth, NV_UNIQUE_NAME) != 0) {
		(void) topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}

	(void) strncpy(tmp, fmristr, fmrilen);
	auth_field = tmp + 6;

	sasname = fmristr + path_offset + 1;

	while (auth_field < (tmp + path_offset)) {
		char *end, *auth_val;
		uint32_t phy;

		if ((end = strchr(auth_field, '=')) == NULL) {
			(void) topo_mod_seterrno(mod, EMOD_FMRI_MALFORM);
			goto err;
		}
		*end = '\0';
		auth_val = end + 1;

		if ((end = strchr(auth_val, ':')) == NULL &&
		    (end = strchr(auth_val, '/')) == NULL) {
			(void) topo_mod_seterrno(mod, EMOD_FMRI_MALFORM);
			goto err;
		}
		*end = '\0';

		if (strcmp(auth_field, FM_FMRI_SAS_TYPE) == 0) {
			(void) nvlist_add_string(auth, auth_field,
			    auth_val);
		} else if (strcmp(auth_field, FM_FMRI_SAS_START_PHY) == 0 ||
		    strcmp(auth_field, FM_FMRI_SAS_END_PHY) == 0) {

			phy = atoi(auth_val);
			(void) nvlist_add_uint32(auth, auth_field, phy);
		}
		auth_field = end + 1;
	}
	(void) nvlist_add_nvlist(fmri, FM_FMRI_AUTHORITY, auth);

	while (i < npairs) {
		nvlist_t *pathcomp;
		uint64_t sasaddr;
		char *end, *addrstr, *estr;

		if (topo_mod_nvalloc(mod, &pathcomp, NV_UNIQUE_NAME) != 0) {
			(void) topo_mod_seterrno(mod, EMOD_NOMEM);
			goto err;
		}
		if ((end = strchr(sasname, '=')) == NULL) {
			(void) topo_mod_seterrno(mod, EMOD_FMRI_MALFORM);
			goto err;
		}
		*end = '\0';
		addrstr = end + 1;

		/*
		 * If this is the last pair, then addrstr will already be
		 * nul-terminated.
		 */
		if (i < (npairs - 1)) {
			if ((end = strchr(addrstr, '/')) == NULL) {
				(void) topo_mod_seterrno(mod,
				    EMOD_FMRI_MALFORM);
				goto err;
			}
			*end = '\0';
		}

		/*
		 * Convert addrstr to a uint64_t
		 */
		errno = 0;
		sasaddr = strtoull(addrstr, &estr, 16);
		if (errno != 0 || *estr != '\0') {
			(void) topo_mod_seterrno(mod, EMOD_FMRI_MALFORM);
			goto err;
		}

		/*
		 * Add both nvpairs to the nvlist and then add the nvlist to
		 * the sas-path nvlist array.
		 */
		if (nvlist_add_string(pathcomp, FM_FMRI_SAS_NAME, sasname) !=
		    0 ||
		    nvlist_add_uint64(pathcomp, FM_FMRI_SAS_ADDR, sasaddr) !=
		    0) {
			(void) topo_mod_seterrno(mod, EMOD_NOMEM);
			goto err;
		}
		sas_path[i++] = pathcomp;
		sasname = end + 1;
	}
	if (nvlist_add_nvlist_array(fmri, FM_FMRI_SAS_PATH, sas_path,
	    npairs) != 0) {
		(void) topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}
	*out = fmri;

	if (sas_path != NULL) {
		for (i = 0; i < npairs; i++)
			nvlist_free(sas_path[i]);

		topo_mod_free(mod, sas_path, npairs * sizeof (nvlist_t *));
	}
	nvlist_free(auth);
	topo_mod_free(mod, tmp, fmrilen + 1);
	return (0);

err:
	topo_mod_dprintf(mod, "%s failed: %s", __func__,
	    topo_strerror(topo_mod_errno(mod)));
	if (sas_path != NULL) {
		for (i = 0; i < npairs; i++)
			nvlist_free(sas_path[i]);

		topo_mod_free(mod, sas_path, npairs * sizeof (nvlist_t *));
	}
	nvlist_free(auth);
	nvlist_free(fmri);
	topo_mod_free(mod, tmp, fmrilen + 1);
	return (-1);
}

/*
 * This method creates a sas-SCHEME FMRI that represents a pathnode.  This is
 * not intended to be called directly, but rather be called via
 * topo_mod_sasfmri()
 */
int
sas_fmri_create(topo_mod_t *mod, tnode_t *node, topo_version_t version,
    nvlist_t *in, nvlist_t **out)
{
	char *nodename;
	uint64_t nodeinst;
	nvlist_t *fmri = NULL, *args, *auth, *saspath[1], *pathcomp = NULL;

	if (version > TOPO_METH_STR2NVL_VERSION)
		return (topo_mod_seterrno(mod, EMOD_VER_NEW));

	if (nvlist_lookup_string(in, TOPO_METH_FMRI_ARG_NAME, &nodename) !=
	    0 ||
	    nvlist_lookup_uint64(in, TOPO_METH_FMRI_ARG_INST, &nodeinst) !=
	    0 ||
	    nvlist_lookup_nvlist(in, TOPO_METH_FMRI_ARG_NVL, &args) != 0 ||
	    nvlist_lookup_nvlist(args, TOPO_METH_FMRI_ARG_AUTH, &auth) != 0) {

		return (topo_mod_seterrno(mod, EMOD_METHOD_INVAL));
	}

	if (topo_mod_nvalloc(mod, &fmri, NV_UNIQUE_NAME) != 0) {
		/* errno set */
		return (-1);
	}

	if (nvlist_add_nvlist(fmri, FM_FMRI_AUTHORITY, auth) != 0 ||
	    nvlist_add_string(fmri, FM_FMRI_SCHEME, FM_FMRI_SCHEME_SAS) != 0 ||
	    nvlist_add_uint8(fmri, FM_FMRI_SAS_VERSION, FM_SAS_SCHEME_VERSION)
	    != 0) {
		(void) topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}

	if (topo_mod_nvalloc(mod, &pathcomp, NV_UNIQUE_NAME) != 0) {
		(void) topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}
	if (nvlist_add_string(pathcomp, FM_FMRI_SAS_NAME, nodename) != 0 ||
	    nvlist_add_uint64(pathcomp, FM_FMRI_SAS_ADDR, nodeinst) != 0) {
		(void) topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}
	saspath[0] = pathcomp;

	if (nvlist_add_nvlist_array(fmri, FM_FMRI_SAS_PATH, saspath, 1) != 0) {
		(void) topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}
	*out = fmri;

	nvlist_free(pathcomp);
	return (0);
err:
	topo_mod_dprintf(mod, "%s failed: %s", __func__,
	    topo_strerror(topo_mod_errno(mod)));
	nvlist_free(pathcomp);
	nvlist_free(fmri);
	return (-1);
}

/*
 * Returns an array of phy error counter data for the given expander phys.
 *
 * This uses libsmp to gather phy link error counter data from the provided
 * expander at smp_path.
 */
static int
sas_get_expander_phy_err_counter(topo_mod_t *mod, tnode_t *node, char *pname,
    char *smp_path, uint32_t start_phy, uint32_t end_phy, uint32_t nphys,
    uint64_t *out)
{
	int ret = -1;

	uint8_t *smp_resp;
	size_t smp_resp_len;
	smp_target_def_t *tdef = NULL;
	smp_target_t *tgt = NULL;
	smp_action_t *axn = NULL;
	smp_result_t result;
	smp_function_t func = SMP_FUNC_REPORT_PHY_ERROR_LOG;

	smp_report_phy_error_log_req_t *el_req;
	smp_report_phy_error_log_resp_t *el_resp;

	if ((tdef = topo_mod_zalloc(mod, sizeof (smp_target_def_t))) == NULL) {
		return (topo_mod_seterrno(mod, EMOD_NOMEM));
	}
	tdef->std_def = smp_path;

	if ((tgt = smp_open(tdef)) == NULL) {
		ret = -1;
		topo_mod_dprintf(mod, "%s: failed to open SMP target",
		    __func__);
		goto err;
	}

	for (uint_t phy = 0; phy < nphys; phy++) {
		axn = smp_action_alloc(func, tgt, 0);
		smp_action_get_request(axn, (void **) &el_req, NULL);
		el_req->srpelr_phy_identifier = start_phy + phy;

		if ((ret = smp_exec(axn, tgt)) != 0) {
			topo_mod_dprintf(mod, "%s: smp_exec failed", __func__);
			smp_action_free(axn);
			goto err;
		}

		smp_action_get_response(axn, &result, (void **) &smp_resp,
		    &smp_resp_len);
		smp_action_free(axn);

		el_resp = (smp_report_phy_error_log_resp_t *)smp_resp;

		/*
		 * When we first do phy discovery we allow an
		 * SMP_RES_PHY_VACANT response. At this point we should be sure
		 * that this phy exists (it's already in the SAS topo digraph),
		 * so any non-ACCEPTED response is an error.
		 */
		if (result != SMP_RES_FUNCTION_ACCEPTED) {
			topo_mod_dprintf(mod, "%s: error in SMP response (%d)",
			    __func__, result);
			goto err;
		}

		/*
		 * Copy out the requested error counter. The values provided
		 * to us by libsmp also need to have their byte order swapped.
		 */
		if (strcmp(pname, TOPO_PROP_SASPORT_INV_DWORD) == 0)
			out[phy] = ntohl(el_resp->srpelr_invalid_dword_count);
		else if (strcmp(pname, TOPO_PROP_SASPORT_RUN_DISP) == 0)
			out[phy] = ntohl(
			    el_resp->srpelr_running_disparity_error_count);
		else if (strcmp(pname, TOPO_PROP_SASPORT_LOSS_SYNC) == 0)
			out[phy] = ntohl(el_resp->srpelr_loss_dword_sync_count);
		else if (strcmp(pname, TOPO_PROP_SASPORT_RESET_PROB) == 0)
			out[phy] = ntohl(
			    el_resp->srpelr_phy_reset_problem_count);
	}
	ret = 0;

err:
	topo_mod_free(mod, tdef, sizeof (smp_target_def_t));
	if (tgt != NULL)
		smp_close(tgt);
	return (ret);
}


/*
 * Returns an array of phy error counter data for the given adapter and phys.
 *
 * This uses libsmhbaapi to gather phy link error counter data.
 */
static int
sas_get_adapter_phy_err_counter(topo_mod_t *mod, tnode_t *node, char *pname,
    char *hba_name, uint32_t hba_port, uint32_t start_phy, uint32_t end_phy,
    uint32_t nphys, uint64_t *out)
{
	HBA_HANDLE handle = -1;
	SMHBA_PHYSTATISTICS phystats;
	SMHBA_SASPHYSTATISTICS sasphystats;
	int ret = -1;

	/*
	 * Iterate through the PHYs on this port and retrieve the
	 * appropriate error counter value based on the property name.
	 */
	if ((handle = HBA_OpenAdapter(hba_name)) == 0) {
		topo_mod_dprintf(mod, "%s: failed to open adapter: %s",
		    __func__, hba_name);
		ret = topo_mod_seterrno(mod, EMOD_UNKNOWN);
		goto err;
	}
	for (uint_t phy = 0; phy < nphys; phy++) {

		(void) memset(&phystats, 0, sizeof (SMHBA_PHYSTATISTICS));
		(void) memset(&sasphystats, 0, sizeof (SMHBA_SASPHYSTATISTICS));
		phystats.SASPhyStatistics = &sasphystats;

		ret = SMHBA_GetPhyStatistics(handle, hba_port, phy, &phystats);
		if (ret != HBA_STATUS_OK) {
			topo_mod_dprintf(mod, "%s: failed to get HBA PHY stats "
			    "for PORT %u PHY %u (ret=%u)", __func__, hba_port,
			    phy, ret);
			ret = topo_mod_seterrno(mod, EMOD_UNKNOWN);
			goto err;
		}
		if (strcmp(pname, TOPO_PROP_SASPORT_INV_DWORD) == 0)
			out[phy] = phystats.
			    SASPhyStatistics->InvalidDwordCount;
		else if (strcmp(pname, TOPO_PROP_SASPORT_RUN_DISP) == 0)
			out[phy] = phystats.
			    SASPhyStatistics->RunningDisparityErrorCount;
		else if (strcmp(pname, TOPO_PROP_SASPORT_LOSS_SYNC) == 0)
			out[phy] = phystats.
			    SASPhyStatistics->LossofDwordSyncCount;
		else if (strcmp(pname, TOPO_PROP_SASPORT_RESET_PROB) == 0)
			out[phy] = phystats.
			    SASPhyStatistics->PhyResetProblemCount;
	}
	ret = 0;
err:
	if (handle != -1)
		HBA_CloseAdapter(handle);
	return (ret);

}

static int
sas_get_target_phy_err_counter(topo_mod_t *mod, tnode_t *node, char *pname,
    uint32_t phy, uint64_t *out)
{
	topo_vertex_t *port_vtx, *tgt_vtx;
	topo_edge_t *tgt_edge;
	tnode_t *tgt_node;
	char *ctdname = NULL, diskpath[PATH_MAX + 1];
	sas_scsi_cache_t *scsi_cache;
	sas_scsi_cache_ent_t *logcache;
	sas_log_page_t *logpage;
	sas_port_param_t *pparam;
	sas_phy_log_descr_t *port_descr;
	hrtime_t curr_ts;
	int err, ret = -1;

	/*
	 * Get the logical-disk propval from the target node.  We use this to
	 * build a device path to the disk, which we need to be able to send
	 * SCSI commands to it.
	 *
	 * To get a pointer to the target node, we first get a pointer to our
	 * associated vertex.  That vertex should have only have one outgoing
	 * edge which should point to the target vertex.
	 */
	port_vtx = topo_node_vertex(node);
	tgt_edge = (topo_edge_t *)topo_list_next(&port_vtx->tvt_outgoing);
	tgt_vtx = tgt_edge->tve_vertex;
	tgt_node = topo_vertex_node(tgt_vtx);

	if (topo_prop_get_string(tgt_node, TOPO_PGROUP_TARGET,
	    TOPO_PROP_TARGET_LOGICAL_DISK, &ctdname, &err) != 0) {
		return (topo_mod_seterrno(mod, err));
	}
	(void) snprintf(diskpath, PATH_MAX + 1, "/dev/rdsk/%s", ctdname);

	if ((scsi_cache = topo_mod_getspecific(mod)) == NULL) {
		return (topo_mod_seterrno(mod, EMOD_UNKNOWN));
	}
	logcache = &scsi_cache->ssc_logpage_cache;
	curr_ts = gethrtime();

	if (strcmp(diskpath, logcache->ssce_devpath) != 0 ||
	    curr_ts - logcache->ssce_ts > PAGE_CACHE_TTL) {

		int fd;

		if ((fd = open(diskpath, O_RDWR|O_NONBLOCK)) < 0) {
			topo_mod_dprintf(mod, "failed to open %s (%s)",
			    diskpath, strerror(errno));
			ret = topo_mod_seterrno(mod, EMOD_UNKNOWN);
			goto err;
		}

		if (scsi_log_sense(mod, fd, PROTOCOL_SPECIFIC_PAGE,
		    logcache->ssce_pagebuf, PAGE_BUFSZ) != 0) {
			/* errno set */
			(void) close(fd);
			goto err;
		}
		(void) close(fd);
		(void) strlcpy(logcache->ssce_devpath, diskpath, PATH_MAX);
		logcache->ssce_ts = gethrtime();
	}
	logpage = (sas_log_page_t *)logcache->ssce_pagebuf;

	/*
	 * Because we're only dealing with SAS targets, we make the assumption
	 * that all ports are narrow ports, hence a 1-1 correllation between
	 * target PHYs and target ports.
	 */
	pparam = (sas_port_param_t *)
	    (logpage->slp_portparam + (sizeof (sas_port_param_t) * phy));

	port_descr = (sas_phy_log_descr_t *)pparam->spp_descr;

	if (strcmp(pname, TOPO_PROP_SASPORT_INV_DWORD) == 0)
		out[0] = BE_32(port_descr->sld_inv_dword);
	else if (strcmp(pname, TOPO_PROP_SASPORT_RUN_DISP) == 0)
		out[0] = BE_32(port_descr->sld_running_disp);
	else if (strcmp(pname, TOPO_PROP_SASPORT_LOSS_SYNC) == 0)
		out[0] = BE_32(port_descr->sld_loss_sync);
	else if (strcmp(pname, TOPO_PROP_SASPORT_RESET_PROB) == 0)
		out[0] = BE_32(port_descr->sld_reset_prob);

	ret = 0;
err:
	topo_mod_strfree(mod, ctdname);
	return (ret);

}

/*
 * Populates PHY link state error counter properties for the given node.
 *
 * This is a common entrypoint for both HBA and expander phys.
 */
int
sas_get_phy_err_counter(topo_mod_t *mod, tnode_t *node, topo_version_t version,
    nvlist_t *in, nvlist_t **out)
{
	uint64_t *pvals = NULL;
	nvlist_t *args, *pargs, *nvl, *fmri = NULL, *auth = NULL;
	char *pname, *aname = NULL, *port_type = NULL;
	uint32_t hba_port, start_phy, end_phy, nphys;
	int err, ret = -1;

	if (version > TOPO_METH_SAS_PHY_ERR_VERSION)
		return (topo_mod_seterrno(mod, ETOPO_METHOD_VERNEW));

	/*
	 * Now look for a private argument list to determine if the invoker is
	 * trying to do a set operation and if so, return an error as this
	 * method only supports get operations.
	 */
	if ((nvlist_lookup_nvlist(in, TOPO_PROP_PARGS, &pargs) == 0) &&
	    nvlist_exists(pargs, TOPO_PROP_VAL_VAL)) {
		topo_mod_dprintf(mod, "%s: set operation not suppported",
		    __func__);
		return (topo_mod_seterrno(mod, EMOD_NVL_INVAL));
	}

	if (nvlist_lookup_nvlist(in, TOPO_PROP_ARGS, &args) != 0 ||
	    nvlist_lookup_string(args, "pname", &pname) != 0) {
		topo_mod_dprintf(mod, "%s: missing pname arg", __func__);
		return (topo_mod_seterrno(mod, EMOD_NVL_INVAL));
	}

	/*
	 * Get the HBA adapter name and port number off of the node, as we need
	 * these to lookup the PHY stats.
	 */
	if (topo_prop_get_string(node, TOPO_PGROUP_SASPORT,
	    TOPO_PROP_SASPORT_ANAME, &aname, &err) != 0) {
		topo_mod_dprintf(mod, "%s: node missing %s prop", __func__,
		    TOPO_PROP_SASPORT_ANAME);
		return (topo_mod_seterrno(mod, err));
	}

	/*
	 * Get the SAS FMRI and then lookup the authority portion in order to
	 * get the start and end PHY numbers.
	 */
	if (topo_node_resource(node, &fmri, &err) != 0) {
		ret = topo_mod_seterrno(mod, err);
		topo_mod_dprintf(mod, "%s: failed to get SAS FMRI", __func__);
		goto err;
	}
	if (nvlist_lookup_nvlist(fmri, FM_FMRI_AUTHORITY, &auth) != 0 ||
	    nvlist_lookup_uint32(auth, FM_FMRI_SAS_START_PHY, &start_phy) !=
	    0 ||
	    nvlist_lookup_uint32(auth, FM_FMRI_SAS_END_PHY, &end_phy) != 0) {
		topo_mod_dprintf(mod, "%s: malformed FMRI authority",
		    __func__);
		ret = topo_mod_seterrno(mod, EMOD_NVL_INVAL);
		goto err;
	}
	nphys = (end_phy - start_phy) + 1;

	/*
	 * Now that we know how many PHYs are on this port, allocate an array
	 * to hold the error counter value for each PHY.
	 */
	if ((pvals = topo_mod_zalloc(mod, sizeof (uint64_t) * nphys)) ==
	    NULL) {
		ret = topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}

	/*
	 * Vector off into the appropriate routine to fetch the PHY error
	 * counters, based on whether we're an initiator, expander or target
	 * port.
	 */
	if (topo_prop_get_string(node, TOPO_PGROUP_SASPORT,
	    TOPO_PROP_SASPORT_TYPE, &port_type, &err) != 0) {
		topo_mod_dprintf(mod, "port node missing %s property",
		    TOPO_PROP_SASPORT_TYPE);
		ret = topo_mod_seterrno(mod, EMOD_UNKNOWN);
		goto err;
	}
	if (strcmp(port_type, TOPO_SASPORT_TYPE_INITIATOR) == 0) {
		if (topo_prop_get_uint32(node, TOPO_PGROUP_SASPORT,
		    TOPO_PROP_SASPORT_APORT, &hba_port, &err) != 0) {
			topo_mod_dprintf(mod, "initiator port node missing "
			    "%s property",  TOPO_PROP_SASPORT_APORT);
			ret = topo_mod_seterrno(mod, EMOD_UNKNOWN);
			goto err;
		}
		if (sas_get_adapter_phy_err_counter(mod, node, pname, aname,
		    hba_port, start_phy, end_phy, nphys, pvals) != 0) {
			goto err;
		}
	} else if (strcmp(port_type, TOPO_SASPORT_TYPE_EXPANDER) == 0) {
		if (sas_get_expander_phy_err_counter(mod, node, pname, aname,
		    start_phy, end_phy, nphys, pvals) != 0) {
			goto err;
		}
	} else if (strcmp(port_type, TOPO_SASPORT_TYPE_TARGET) == 0) {
		if (sas_get_target_phy_err_counter(mod, node, pname, start_phy,
		    pvals) != 0) {
			goto err;
		}
	}

	if (topo_mod_nvalloc(mod, &nvl, NV_UNIQUE_NAME) != 0 ||
	    nvlist_add_string(nvl, TOPO_PROP_VAL_NAME, pname) != 0 ||
	    nvlist_add_uint32(nvl, TOPO_PROP_VAL_TYPE, TOPO_TYPE_UINT64_ARRAY)
	    != 0 ||
	    nvlist_add_uint64_array(nvl, TOPO_PROP_VAL_VAL, pvals, nphys)
	    != 0) {
		topo_mod_dprintf(mod, "Failed to allocate 'out' nvlist");
		nvlist_free(nvl);
		ret = topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}
	*out = nvl;

	ret = 0;
err:
	if (ret != 0)
		topo_mod_dprintf(mod, "%s: failed to get PHY error counters "
		    "for %s=%" PRIx64, __func__, topo_node_name(node),
		    topo_node_instance(node));

	nvlist_free(fmri);
	if (pvals != NULL) {
		topo_mod_free(mod, pvals, sizeof (uint64_t) * nphys);
	}
	topo_mod_strfree(mod, aname);
	topo_mod_strfree(mod, port_type);
	return (ret);
}

/*
 * Helper function called by sas_get_phy_link_rate() to retrieve PHY link
 * transmission rates from the specified adapter port PHY(s).
 *
 * This uses libsmhbaapi to gather phy link attributes.  Libsmhbaapi
 * sources its data from the "phy-info" devinfo property that's created
 * by the HBA driver for each PHY.
 *
 * Returns 0 on success.  Returns -1 and sets topo errno on failure.
 */
static int
sas_get_adapter_phy_link_rate(topo_mod_t *mod, tnode_t *node, char *pname,
    char *hba_name, uint32_t hba_port, uint32_t start_phy, uint32_t end_phy,
    uint32_t nphys, uint32_t *out)
{
	HBA_HANDLE handle = -1;
	SMHBA_SAS_PHY phyattrs;
	int ret = -1;

	/*
	 * Iterate through the PHYs on this port and retrieve the
	 * appropriate PHY link rate status based on the property name.
	 */
	if ((handle = HBA_OpenAdapter(hba_name)) == 0) {
		topo_mod_dprintf(mod, "%s: failed to open adapter: %s",
		    __func__, hba_name);
		ret = topo_mod_seterrno(mod, EMOD_UNKNOWN);
		goto err;
	}
	for (uint_t phy = 0; phy < nphys; phy++) {

		(void) memset(&phyattrs, 0, sizeof (SMHBA_SAS_PHY));

		ret = SMHBA_GetSASPhyAttributes(handle, hba_port, phy,
		    &phyattrs);
		if (ret != HBA_STATUS_OK) {
			topo_mod_dprintf(mod, "%s: failed to get HBA PHY attrs "
			    "for PORT %u PHY %u (ret=%u)", __func__, hba_port,
			    phy, ret);
			ret = topo_mod_seterrno(mod, EMOD_UNKNOWN);
			goto err;
		}
		if (strcmp(pname, TOPO_PROP_SASPORT_MAX_RATE) == 0)
			out[phy] = phyattrs.HardwareMaxLinkRate;
		else if (strcmp(pname, TOPO_PROP_SASPORT_PROG_RATE) == 0)
			out[phy] = phyattrs.ProgrammedMaxLinkRate;
		else if (strcmp(pname, TOPO_PROP_SASPORT_NEG_RATE) == 0)
			out[phy] = phyattrs.NegotiatedLinkRate;
	}
	ret = 0;
err:
	if (handle != -1)
		HBA_CloseAdapter(handle);
	return (ret);

}

/*
 * Helper function called by sas_get_phy_link_rate() to retrieve PHY link
 * transmission rates from the specified expander port PHY(s).
 *
 * Returns 0 on success.  Returns -1 and sets topo errno on failure.
 */
static int
sas_get_expander_phy_link_rate(topo_mod_t *mod, tnode_t *node, char *pname,
    char *smp_path, uint32_t start_phy, uint32_t end_phy, uint32_t nphys,
    uint32_t *out)
{
	smp_target_t *tgt = NULL;
	smp_target_def_t *tdef = NULL;
	int ret = -1;

	if ((tdef = topo_mod_zalloc(mod, sizeof (smp_target_def_t))) == NULL) {
		return (topo_mod_seterrno(mod, EMOD_NOMEM));
	}
	tdef->std_def = smp_path;

	if ((tgt = smp_open(tdef)) == NULL) {
		topo_mod_dprintf(mod, "%s: failed to open SMP target",
		    __func__);
		ret = topo_mod_seterrno(mod, EMOD_UNKNOWN);
		goto err;
	}

	for (uint_t phy = 0; phy < nphys; phy++) {
		smp_function_t func;
		smp_result_t result;
		smp_action_t *axn = NULL;
		smp_discover_req_t *disc_req = NULL;
		smp_discover_resp_t *disc_resp = NULL;
		size_t smp_resp_len;
		uint8_t *smp_resp;

		func = SMP_FUNC_DISCOVER;
		axn = smp_action_alloc(func, tgt, 0);
		smp_action_get_request(axn, (void **) &disc_req, NULL);
		disc_req->sdr_phy_identifier = start_phy + phy;

		if (smp_exec(axn, tgt) != 0) {
			topo_mod_dprintf(mod, "%s: smp_exec failed", __func__);
			ret = topo_mod_seterrno(mod, EMOD_UNKNOWN);
			smp_action_free(axn);
			goto err;
		}

		smp_action_get_response(axn, &result, (void **)&smp_resp,
		    &smp_resp_len);
		smp_action_free(axn);

		disc_resp = (smp_discover_resp_t *)smp_resp;

		/*
		 * When we first do PHY discovery we allow an
		 * SMP_RES_PHY_VACANT response. At this point we should be sure
		 * that this phy exists (it's already in the SAS topo digraph),
		 * so any non-ACCEPTED response is an error.
		 */
		if (result != SMP_RES_FUNCTION_ACCEPTED) {
			topo_mod_dprintf(mod, "%s: error in SMP response (%d)",
			    __func__, result);
			ret = topo_mod_seterrno(mod, EMOD_UNKNOWN);
			goto err;
		}
		if (strcmp(pname, TOPO_PROP_SASPORT_MAX_RATE) == 0)
			out[phy] = disc_resp->sdr_prog_max_phys_link_rate;
		else if (strcmp(pname, TOPO_PROP_SASPORT_PROG_RATE) == 0)
			out[phy] = disc_resp->sdr_prog_max_phys_link_rate;
		else if (strcmp(pname, TOPO_PROP_SASPORT_NEG_RATE) == 0)
			out[phy] = disc_resp->sdr_negotiated_logical_link_rate;
	}
	ret = 0;

err:
	topo_mod_free(mod, tdef, sizeof (smp_target_def_t));
	smp_close(tgt);
	return (ret);
}

/*
 * Helper function called by sas_get_phy_link_rate() to retrieve PHY link
 * transmission rates from the specified target port PHY.
 *
 * Returns 0 on success.  Returns -1 and sets topo errno on failure.
 */
static int
sas_get_target_phy_link_rate(topo_mod_t *mod, tnode_t *node, char *pname,
    uint32_t phy, uint32_t *out)
{
	topo_vertex_t *port_vtx, *tgt_vtx;
	topo_edge_t *tgt_edge;
	tnode_t *tgt_node;
	char *ctdname = NULL, diskpath[PATH_MAX + 1];
	uint16_t bdlen;
	sas_scsi_cache_t *scsi_cache;
	sas_scsi_cache_ent_t *modecache;
	scsi_mode10_hdr_t *modehdr;
	sas_phys_disc_mode_page_t *modepage;
	sas_phy_descriptor_t *phy_descr;
	hrtime_t curr_ts;
	int err, ret = -1;

	/*
	 * Get the logical-disk propval from the target node.  We use this to
	 * build a device path to the disk, which we need to be able to send
	 * SCSI commands to it.
	 *
	 * To get a pointer to the target node, we first get a pointer to our
	 * associated vertex.  That vertex should have only have one outgoing
	 * edge which should point to the target vertex.
	 */
	port_vtx = topo_node_vertex(node);
	tgt_edge = (topo_edge_t *)topo_list_next(&port_vtx->tvt_outgoing);
	tgt_vtx = tgt_edge->tve_vertex;
	tgt_node = topo_vertex_node(tgt_vtx);

	if (topo_prop_get_string(tgt_node, TOPO_PGROUP_TARGET,
	    TOPO_PROP_TARGET_LOGICAL_DISK, &ctdname, &err) != 0) {
		return (topo_mod_seterrno(mod, err));
	}
	(void) snprintf(diskpath, PATH_MAX + 1, "/dev/rdsk/%s", ctdname);

	if ((scsi_cache = topo_mod_getspecific(mod)) == NULL) {
		return (topo_mod_seterrno(mod, EMOD_UNKNOWN));
	}
	modecache = &scsi_cache->ssc_modepage_cache;
	curr_ts = gethrtime();

	if (strcmp(diskpath, modecache->ssce_devpath) != 0 ||
	    curr_ts - modecache->ssce_ts > PAGE_CACHE_TTL) {

		int fd;

		if ((fd = open(diskpath, O_RDWR |O_NONBLOCK)) < 0) {
			topo_mod_dprintf(mod, "failed to open %s (%s)",
			    diskpath, strerror(errno));
			ret = topo_mod_seterrno(mod, EMOD_UNKNOWN);
			goto err;
		}

		if (scsi_mode_sense(mod, fd, ENHANCED_PHY_CONTROL_PAGE,
		    0x1, modecache->ssce_pagebuf, PAGE_BUFSZ) != 0) {
			/* errno set */
			(void) close(fd);
			goto err;
		}
		(void) close(fd);
		(void) strlcpy(modecache->ssce_devpath, diskpath, PATH_MAX);
		modecache->ssce_ts = gethrtime();
	}
	modehdr = (scsi_mode10_hdr_t *)modecache->ssce_pagebuf;
	bdlen = BE_16(modehdr->smh10_bdlen);

	modepage = (sas_phys_disc_mode_page_t *)
	    (modecache->ssce_pagebuf + sizeof (scsi_mode10_hdr_t) + bdlen);

	/*
	 * XXX maybe add an assert that phy <= (spdm_nphys -1) ?
	 */
	phy_descr = (sas_phy_descriptor_t *)
	    (modepage->spdm_descr + (sizeof (sas_phy_descriptor_t) * phy));

	if (strcmp(pname, TOPO_PROP_SASPORT_MAX_RATE) == 0)
		out[0] = phy_descr->spde_hw_max_rate;
	else if (strcmp(pname, TOPO_PROP_SASPORT_PROG_RATE) == 0)
		out[0] = phy_descr->spde_prog_max_rate;
	else if (strcmp(pname, TOPO_PROP_SASPORT_NEG_RATE) == 0)
		out[0] = phy_descr->spde_neg_rate;

	ret = 0;
err:
	topo_mod_strfree(mod, ctdname);
	return (ret);

}

/*
 * Property method for TOPO_PROP_SASPORT_{MAX,PROG,NEG}_RATE
 */
int
sas_get_phy_link_rate(topo_mod_t *mod, tnode_t *node, topo_version_t version,
    nvlist_t *in, nvlist_t **out)
{
	uint32_t *pvals = NULL;
	nvlist_t *args, *pargs, *nvl, *fmri = NULL, *auth = NULL;
	char *pname, *aname = NULL, *port_type = NULL;
	uint32_t hba_port, start_phy, end_phy, nphys;
	int err, ret = -1;

	if (version > TOPO_METH_SAS_PHY_ERR_VERSION)
		return (topo_mod_seterrno(mod, ETOPO_METHOD_VERNEW));

	/*
	 * Look for a private argument list to determine if the invoker is
	 * trying to do a set operation and if so, return an error as this
	 * method only supports get operations.
	 */
	if ((nvlist_lookup_nvlist(in, TOPO_PROP_PARGS, &pargs) == 0) &&
	    nvlist_exists(pargs, TOPO_PROP_VAL_VAL)) {
		topo_mod_dprintf(mod, "%s: set operation not suppported",
		    __func__);
		return (topo_mod_seterrno(mod, EMOD_NVL_INVAL));
	}

	if (nvlist_lookup_nvlist(in, TOPO_PROP_ARGS, &args) != 0 ||
	    nvlist_lookup_string(args, "pname", &pname) != 0) {
		topo_mod_dprintf(mod, "%s: missing pname arg", __func__);
		return (topo_mod_seterrno(mod, EMOD_NVL_INVAL));
	}

	/*
	 * Get the HBA adapter name and port number off of the node, as we need
	 * these to lookup the PHY stats.
	 */
	if (topo_prop_get_string(node, TOPO_PGROUP_SASPORT,
	    TOPO_PROP_SASPORT_ANAME, &aname, &err) != 0) {
		topo_mod_dprintf(mod, "%s: node missing %s prop", __func__,
		    TOPO_PROP_SASPORT_ANAME);
		return (topo_mod_seterrno(mod, err));
	}

	/*
	 * Get the SAS FMRI and then lookup the authority portion in order to
	 * get the start and end PHY numbers.
	 */
	if (topo_node_resource(node, &fmri, &err) != 0) {
		ret = topo_mod_seterrno(mod, err);
		topo_mod_dprintf(mod, "%s: failed to get SAS FMRI", __func__);
		goto err;
	}
	if (nvlist_lookup_nvlist(fmri, FM_FMRI_AUTHORITY, &auth) != 0 ||
	    nvlist_lookup_uint32(auth, FM_FMRI_SAS_START_PHY, &start_phy) !=
	    0 ||
	    nvlist_lookup_uint32(auth, FM_FMRI_SAS_END_PHY, &end_phy) != 0) {
		topo_mod_dprintf(mod, "%s: malformed FMRI authority",
		    __func__);
		ret = topo_mod_seterrno(mod, EMOD_NVL_INVAL);
		goto err;
	}
	nphys = (end_phy - start_phy) + 1;

	/*
	 * Now that we know how many PHYs are on this port, allocate an array
	 * to hold the link rate state value for each PHY.
	 */
	if ((pvals = topo_mod_zalloc(mod, sizeof (uint32_t) * nphys)) ==
	    NULL) {
		ret = topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}

	/*
	 * Vector off into the appropriate routine based on whether we're an
	 * initiator, expander or target port.
	 */
	if (topo_prop_get_string(node, TOPO_PGROUP_SASPORT,
	    TOPO_PROP_SASPORT_TYPE, &port_type, &err) != 0) {
		topo_mod_dprintf(mod, "port node missing %s property",
		    TOPO_PROP_SASPORT_TYPE);
		ret = topo_mod_seterrno(mod, EMOD_UNKNOWN);
		goto err;
	}
	if (strcmp(port_type, TOPO_SASPORT_TYPE_INITIATOR) == 0) {
		if (topo_prop_get_uint32(node, TOPO_PGROUP_SASPORT,
		    TOPO_PROP_SASPORT_APORT, &hba_port, &err) != 0) {
			topo_mod_dprintf(mod, "initiator port node missing "
			    "%s property",  TOPO_PROP_SASPORT_APORT);
			ret = topo_mod_seterrno(mod, EMOD_UNKNOWN);
			goto err;
		}
		if (sas_get_adapter_phy_link_rate(mod, node, pname, aname,
		    hba_port, start_phy, end_phy, nphys, pvals) != 0) {
			/* errno set */
			goto err;
		}
	} else if (strcmp(port_type, TOPO_SASPORT_TYPE_EXPANDER) == 0) {
		if (sas_get_expander_phy_link_rate(mod, node, pname, aname,
		    start_phy, end_phy, nphys, pvals) != 0) {
			/* errno set */
			goto err;
		}
	} else if (strcmp(port_type, TOPO_SASPORT_TYPE_TARGET) == 0) {
		if (sas_get_target_phy_link_rate(mod, node, pname, start_phy,
		    pvals) != 0) {
			/* errno set */
			goto err;
		}
	}

	if (topo_mod_nvalloc(mod, &nvl, NV_UNIQUE_NAME) != 0 ||
	    nvlist_add_string(nvl, TOPO_PROP_VAL_NAME, pname) != 0 ||
	    nvlist_add_uint32(nvl, TOPO_PROP_VAL_TYPE, TOPO_TYPE_UINT32_ARRAY)
	    != 0 ||
	    nvlist_add_uint32_array(nvl, TOPO_PROP_VAL_VAL, pvals, nphys)
	    != 0) {
		topo_mod_dprintf(mod, "Failed to allocate 'out' nvlist");
		nvlist_free(nvl);
		ret = topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}
	*out = nvl;

	ret = 0;
err:
	if (ret != 0)
		topo_mod_dprintf(mod, "%s: failed to get PHY link rate(s) "
		    "for %s=%" PRIx64, __func__, topo_node_name(node),
		    topo_node_instance(node));

	nvlist_free(fmri);
	if (pvals != NULL) {
		topo_mod_free(mod, pvals, sizeof (uint32_t) * nphys);
	}
	topo_mod_strfree(mod, aname);
	topo_mod_strfree(mod, port_type);
	return (ret);
}
