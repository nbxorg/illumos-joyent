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

/*
 * The sas FMRI scheme is intended to be used in conjuction with a
 * digraph-based topology to represent a SAS fabric.
 *
 * There are four types of vertices in the topology:
 *
 * initiator
 * ---------
 * An initiator is a device on the SAS fabric that originates SCSI commands.
 * Typically this is a SAS host-bus adapter (HBA) which can be built onto the
 * system board or be part of a PCIe add-in card.
 *
 *   initiator properties
 *   --------------------
 *   Initiator nodes are populated with the following public properties:
 *   - manufacturer
 *   - model
 *   - devfs-name
 *   - location
 *   - hc fmri
 *
 * port
 * ----
 * A port is a logical construct that represents a grouping of one or more
 * PHYs.  A port with one PHY is known as a narrow port.  An example of a
 * narrow port would be the connection from an expander to a target device.
 * A port with more than one PHY is known as a wide port.  A typical example
 * of a wide port would be the connection from an initiator to an exander
 * (typically 4 or 8 PHYs wide).
 *
 * XXX - add description of port node properties
 *
 * target
 * ------
 * A target (or end-device) represents the device that is receiving
 * SCSI commands from the an initiator.   Examples include disks and SSDs as
 * well as SMP and SES management devices.  SES and SMP targets would
 * be connected to an expander.  Disk/SSD targets can be connected to an
 * expander or directly attached (via a narrow port) to an initiator.
 *
 *   target properties
 *   -----------------
 *   Target nodes are populated with the following public properties.
 *   - manufacturer
 *   - model
 *   - serial-number
 *   - location
 *   - logical-disk
 *   - devfs-path
 *   - devid
 *   - speed-in-rpm
 *   - hc-fmri
 *
 * XXX - It'd be really cool if we could check for a ZFS pool config and
 * try to match the target to a leaf vdev and include the zfs-scheme FMRI of
 * that vdev as a property on this node.
 *
 * expander
 * --------
 * An expander acts as both a port multiplexer and expander routing signals
 * between one or more initiators and one or more targets or possibly a
 * second layer of downstream expanders, depending on the size of the fabric.
 * The SAS specification optionally allows for up to two levels of expanders
 * between the initiator(s) and target(s).
 *
 * XXX - add description of expander node properties
 *
 * Version 0 sas FMRI scheme
 * -------------------------
 * Two types of resources can be represented in the sas FMRI scheme: paths
 * and pathnodes.  The "type" field in the authority portion of the FMRI
 * denotes whether the FMRI indentifies a pathnode or path:
 *
 * e.g.
 * sas://type=path/....
 * sas://type=pathnode/....
 *
 * Path
 * ----
 * The first resource type is a path, which represents a unique path from a
 * given initiator to a given target.  Hence, the first two node/instance pairs
 * are always an initiator and port and the last two pairs are always a port
 * and a target. In between there may be one or two sets of expander and port
 * pairs.
 *
 * e.g.
 * sas://<auth>/initiator=<inst>/<port>=<inst>/.../port=<inst>/target=<inst>
 *
 * Node instance numbers are based on the local SAS address of the underlying
 * component.  Each initiator, expander and target will have a unique[1] SAS
 * address.  And each port from an initiator or to a target will also have a
 * unique SAS address.  Note that expander ports are not individually
 * addressed, thus the instance number shall be the SAS address of the
 * expander, itself.
 *
 * [1] The SAS address will be unique within a given SAS fabric (domain)
 *
 * The nvlist representation of the FMRI consists of two nvpairs:
 *
 * name               type                   value
 * ----               ----                   -----
 * sas-fmri-version   DATA_TYPE_UINT8        0
 * sas-path           DATA_TYPE_NVLIST_ARRAY see below
 *
 * sas-path is an array of nvlists where each nvlist contains the following
 * nvpairs:
 *
 * name               type                   value
 * ----               ----                   -----
 * sas-name           DATA_TYPE_STRING       (initiator|port|expander|target)
 * sas-id             DATA_TYPE_UINT64       SAS address (see above)
 *
 *
 * Pathnode
 * --------
 * The second resource type in the sas FMRI scheme is a pathnode, which
 * represents a single node in the underlying graph topology.  In this form,
 * the FMRI consists of a single sas-name/sas-id pair.  In order to
 * differentiate the FMRIs for expander "port" nodes, which will share the same
 * SAS address as the expander, the range of PHYs associated with the port will
 * be added to the authority portion of the FMRI.  For expander ports that
 * connect directly to a target device, this will be a narrow port that spans a
 * single PHY:
 *
 * e.g.
 *
 * sas://type=pathnode:start-phy=0:end-phy=0/port=500304801861347f
 * sas://type=pathnode:start-phy=1:end-phy=1/port=500304801861347f
 *
 * For expander ports that connect to another expander, this will be a wide
 * port that will span a range of phys (typically 4 or 8 wide)
 *
 * e.g.
 *
 * sas://type=pathnode:start_phy=0:end_phy=7/port=500304801861347f
 *
 * Overview of SAS Topology Generation
 * -----------------------------------
 * The SAS topology is iterated using this high-level logic:
 *
 * 1) Each HBA is discovered.
 * 2) Each SAS port on each HBA is added to a list of ports that need to be
 *    further discovered (search_list).
 * 3) Create a digraph vertex for every device discovered. Some information is
 *    stored with each vertex like its local WWN, attached WWN, and port
 *    attributes.
 * 4) Iterate through each vertex drawing edges between all connected
 *    vertices. The connections are determined by matching local/attached WWN
 *    pairs. E.g. a disk with an attached WWN of 0xDEADBEEF and an HBA with a
 *    local WWN of 0xDEADBEEF are connected.
 */
#include <libnvpair.h>
#include <fm/topo_mod.h>

#include <sys/fm/protocol.h>
#include <sys/types.h>

#include <topo_digraph.h>
#include <topo_sas.h>
#include <topo_method.h>
#include <topo_subr.h>
#include "sas.h"

#include <smhbaapi.h>
#include <scsi/libsmp.h>
#include <libdevinfo.h>
#include <scsi/plugins/ses/framework/ses2.h>

/* Methods for the root sas topo node. */
extern int sas_fmri_nvl2str(topo_mod_t *, tnode_t *, topo_version_t,
    nvlist_t *, nvlist_t **);
extern int sas_fmri_str2nvl(topo_mod_t *, tnode_t *, topo_version_t,
    nvlist_t *, nvlist_t **);
extern int sas_fmri_create(topo_mod_t *, tnode_t *, topo_version_t,
    nvlist_t *, nvlist_t **);

/* Methods for child sas topo nodes. */
extern int sas_dev_fmri(topo_mod_t *, tnode_t *, topo_version_t,
    nvlist_t *, nvlist_t **);
extern int sas_hc_fmri(topo_mod_t *, tnode_t *, topo_version_t,
    nvlist_t *, nvlist_t **);
extern int sas_device_props_set(topo_mod_t *, tnode_t *, topo_version_t,
    nvlist_t *, nvlist_t **);
extern int sas_get_phy_err_counter(topo_mod_t *, tnode_t *, topo_version_t,
    nvlist_t *, nvlist_t **);
extern int sas_get_phy_link_rate(topo_mod_t *, tnode_t *, topo_version_t,
    nvlist_t *, nvlist_t **);

static const topo_method_t sas_root_methods[] = {
	{ TOPO_METH_NVL2STR, TOPO_METH_NVL2STR_DESC, TOPO_METH_NVL2STR_VERSION,
	    TOPO_STABILITY_INTERNAL, sas_fmri_nvl2str },
	{ TOPO_METH_STR2NVL, TOPO_METH_STR2NVL_DESC, TOPO_METH_STR2NVL_VERSION,
	    TOPO_STABILITY_INTERNAL, sas_fmri_str2nvl },
	{ TOPO_METH_FMRI, TOPO_METH_FMRI_DESC, TOPO_METH_FMRI_VERSION,
	    TOPO_STABILITY_INTERNAL, sas_fmri_create },
	{ NULL }
};

static const topo_method_t sas_initiator_methods[] = {
	{ TOPO_METH_SAS2DEV, TOPO_METH_SAS2DEV_DESC, TOPO_METH_SAS2DEV_VERSION,
	    TOPO_STABILITY_INTERNAL, sas_dev_fmri },
	{ TOPO_METH_SAS2HC, TOPO_METH_SAS2HC_DESC, TOPO_METH_SAS2HC_VERSION,
	    TOPO_STABILITY_INTERNAL, sas_hc_fmri },
	{ TOPO_METH_SAS_DEV_PROP, TOPO_METH_SAS_DEV_PROP_DESC,
	    TOPO_METH_SAS_DEV_PROP_VERSION, TOPO_STABILITY_INTERNAL,
	    sas_device_props_set },
	{ TOPO_METH_SAS2DEV, TOPO_METH_SAS2DEV_DESC,
	    TOPO_METH_SAS2DEV_VERSION, TOPO_STABILITY_INTERNAL,
	    sas_dev_fmri },
	{ NULL }
};

static const topo_method_t sas_expander_methods[] = {
	{ TOPO_METH_SAS2DEV, TOPO_METH_SAS2DEV_DESC, TOPO_METH_SAS2DEV_VERSION,
	    TOPO_STABILITY_INTERNAL, sas_dev_fmri },
	{ NULL }
};

static const topo_method_t sas_target_methods[] = {
	{ TOPO_METH_SAS2DEV, TOPO_METH_SAS2DEV_DESC, TOPO_METH_SAS2DEV_VERSION,
	    TOPO_STABILITY_INTERNAL, sas_dev_fmri },
	{ TOPO_METH_SAS2HC, TOPO_METH_SAS2HC_DESC, TOPO_METH_SAS2HC_VERSION,
	    TOPO_STABILITY_INTERNAL, sas_hc_fmri },
	{ TOPO_METH_SAS_DEV_PROP, TOPO_METH_SAS_DEV_PROP_DESC,
	    TOPO_METH_SAS_DEV_PROP_VERSION, TOPO_STABILITY_INTERNAL,
	    sas_device_props_set },
	{ TOPO_METH_SAS2DEV, TOPO_METH_SAS2DEV_DESC,
	    TOPO_METH_SAS2DEV_VERSION, TOPO_STABILITY_INTERNAL,
	    sas_dev_fmri },
	{ NULL }
};

static const topo_method_t sas_port_methods[] = {
	{ TOPO_METH_SAS_PHY_ERR, TOPO_METH_SAS_PHY_ERR_DESC,
	    TOPO_METH_SAS_PHY_ERR_VERSION, TOPO_STABILITY_INTERNAL,
	    sas_get_phy_err_counter },
	{ TOPO_METH_SAS_LINK_RATE, TOPO_METH_SAS_LINK_RATE_DESC,
	    TOPO_METH_SAS_LINK_RATE_VERSION, TOPO_STABILITY_INTERNAL,
	    sas_get_phy_link_rate },
	{ NULL }
};

static int sas_enum(topo_mod_t *, tnode_t *, const char *, topo_instance_t,
    topo_instance_t, void *, void *);
static void sas_release(topo_mod_t *, tnode_t *);

static const topo_modops_t sas_ops =
	{ sas_enum, sas_release };

static const topo_modinfo_t sas_info =
	{ "sas", FM_FMRI_SCHEME_SAS, SAS_VERSION, &sas_ops };

int
sas_init(topo_mod_t *mod, topo_version_t version)
{
	sas_scsi_cache_t *scsi_cache;
	HBA_STATUS ret;

	if (getenv("TOPOSASDEBUG"))
		topo_mod_setdebug(mod);

	topo_mod_dprintf(mod, "initializing sas builtin");

	if (version != SAS_VERSION)
		return (topo_mod_seterrno(mod, EMOD_VER_NEW));

	if ((ret = HBA_LoadLibrary()) != HBA_STATUS_OK) {
		topo_mod_dprintf(mod, "failed to load HBA library (ret=%u)",
		    ret);
		return (-1);
	}

	if ((scsi_cache = topo_mod_zalloc(mod, sizeof (sas_scsi_cache_t))) ==
	    NULL) {
		return (topo_mod_seterrno(mod, EMOD_NOMEM));
	}
	topo_mod_setspecific(mod, scsi_cache);

	if (topo_mod_register(mod, &sas_info, TOPO_VERSION) != 0) {
		topo_mod_dprintf(mod, "failed to register sas_info: %s",
		    topo_mod_errmsg(mod));
		return (-1);
	}

	return (0);
}

void
sas_fini(topo_mod_t *mod)
{
	sas_scsi_cache_t *scsi_cache;

	if ((scsi_cache = topo_mod_getspecific(mod)) != NULL) {
		topo_mod_free(mod, scsi_cache, sizeof (sas_scsi_cache_t));
	}
	topo_mod_unregister(mod);
	(void) HBA_FreeLibrary();
}

static topo_pgroup_info_t protocol_pgroup = {
	TOPO_PGROUP_PROTOCOL,
	TOPO_STABILITY_PRIVATE,
	TOPO_STABILITY_PRIVATE,
	1
};

struct sas_phy_info {
	uint32_t	start_phy;
	uint32_t	end_phy;
};

/*
 * Some hardware information like manufacturer, serial number, etc. are not
 * available via SMP or libsmhbaapi. This data is provided by the HC module,
 * however. The HC module does not have its tree constructed when this sas
 * module runs, so we need to register callbacks for these properties.
 *
 * The callbacks will look up the device specific properties in the HC tree
 * and copy them alongside the sas topo node.
 */
static int
sas_prop_method_register(topo_mod_t *mod, tnode_t *tn, const char *pgname)
{
	const topo_method_t *propmethods;
	const char *nodename = topo_node_name(tn);
	topo_instance_t nodeinst = topo_node_instance(tn);
	nvlist_t *nvl = NULL;
	int err, ret = -1;

	if (strcmp(topo_node_name(tn), TOPO_VTX_INITIATOR) == 0)
		propmethods = sas_initiator_methods;
	else if (strcmp(topo_node_name(tn), TOPO_VTX_PORT) == 0)
		propmethods = sas_port_methods;
	else if (strcmp(topo_node_name(tn), TOPO_VTX_EXPANDER) == 0)
		propmethods = sas_expander_methods;
	else if (strcmp(topo_node_name(tn), TOPO_VTX_TARGET) == 0)
		propmethods = sas_target_methods;

	if (topo_method_register(mod, tn, propmethods) != 0) {
		topo_mod_dprintf(mod, "failed to register fmri"
		    "methods for %s=%" PRIx64, topo_node_name(tn),
		    topo_node_instance(tn));
		/* errno set */
		goto err;
	}

	if (topo_mod_nvalloc(mod, &nvl, NV_UNIQUE_NAME) != 0) {
		return (topo_mod_seterrno(mod, EMOD_NOMEM));
	}

	if (strcmp(pgname, TOPO_PGROUP_TARGET) == 0) {
		topo_type_t ptype = TOPO_TYPE_STRING;

		fnvlist_add_string(nvl, "pname", TOPO_PROP_TARGET_HC_FMRI);

		if (topo_prop_method_register(tn, pgname,
		    TOPO_PROP_TARGET_HC_FMRI, TOPO_TYPE_STRING,
		    TOPO_METH_SAS2HC, nvl, &err) != 0) {
			topo_mod_dprintf(mod, "Failed to set "
			    "up hc fmri cb on %s=%" PRIx64 " (%s)",
			    nodename, nodeinst, topo_strerror(err));
			goto err;
		}

		const char *props[] = {
		    TOPO_PROP_TARGET_DEVFS_PATH,
		    TOPO_PROP_TARGET_DEVID,
		    TOPO_PROP_TARGET_LABEL,
		    TOPO_PROP_TARGET_LOGICAL_DISK,
		    TOPO_PROP_TARGET_MANUF,
		    TOPO_PROP_TARGET_MODEL,
		    TOPO_PROP_TARGET_SERIAL
		};

		for (uint_t i = 0; i < sizeof (props) / sizeof (props[0]);
		    i++) {
			fnvlist_add_string(nvl, "pname", props[i]);

			if (topo_prop_method_register(tn, pgname,
			    props[i], ptype, TOPO_METH_SAS_DEV_PROP, nvl,
			    &err) != 0) {
				topo_mod_dprintf(mod, "Failed to register "
				    "up propmethod on %s=%" PRIx64 " (%s)",
				    nodename, nodeinst, topo_strerror(err));
				goto err;
			}
			if (topo_prop_setnonvolatile(tn, pgname, props[i],
			    &err) != 0) {
				topo_mod_dprintf(mod, "Failed to nonvolatile"
				    "flag for prop %s/%s", pgname, props[i]);
				goto err;
			}
		}
		if (topo_prop_method_register(tn, pgname,
		    TOPO_PROP_TARGET_DEV_FMRI, ptype, TOPO_METH_SAS2DEV,
		    nvl, &err) != 0) {
			topo_mod_dprintf(mod, "Failed to register propmethod "
			    "on %s=%" PRIx64 " (%s)", nodename, nodeinst,
			    topo_strerror(err));
			goto err;
		}
		if (topo_prop_setnonvolatile(tn, pgname,
		    TOPO_PROP_TARGET_DEV_FMRI, &err) != 0) {
			topo_mod_dprintf(mod, "Failed to nonvolatile flag for "
			    "prop %s/%s", pgname, TOPO_PROP_TARGET_DEV_FMRI);
			goto err;
		}

	} else if (strcmp(pgname, TOPO_PGROUP_INITIATOR) == 0) {
		topo_type_t ptype = TOPO_TYPE_STRING;

		fnvlist_add_string(nvl, "pname", TOPO_PROP_INITIATOR_FMRI);

		if (topo_prop_method_register(tn, pgname,
		    TOPO_PROP_INITIATOR_FMRI,
		    TOPO_TYPE_STRING, TOPO_METH_SAS2HC, nvl, &err) != 0) {
			topo_mod_dprintf(mod, "Failed to set "
			    "hc fmri cb on %s=%" PRIx64 " (%s)",
			    nodename, nodeinst, topo_strerror(err));
			goto err;
		}

		const char *props[] = {
		    TOPO_PROP_INITIATOR_MANUF,
		    TOPO_PROP_INITIATOR_MODEL,
		    TOPO_PROP_INITIATOR_LABEL
		};

		for (uint_t i = 0; i < sizeof (props) / sizeof (props[0]);
		    i++) {
			fnvlist_add_string(nvl, "pname", props[i]);

			if (topo_prop_method_register(tn, pgname, props[i],
			    ptype, TOPO_METH_SAS_DEV_PROP, nvl, &err) != 0) {
				topo_mod_dprintf(mod, "Failed to set "
				    "up prop cb on %s=%" PRIx64 " (%s)",
				    nodename, nodeinst, topo_strerror(err));
				goto err;
			}
			if (topo_prop_setnonvolatile(tn, pgname, props[i],
			    &err) != 0) {
				topo_mod_dprintf(mod, "Failed to nonvolatile"
				    "flag for prop %s/%s", pgname, props[i]);
				goto err;
			}
		}
		if (topo_prop_method_register(tn, pgname,
		    TOPO_PROP_INITIATOR_DEV_FMRI, ptype, TOPO_METH_SAS2DEV,
		    nvl, &err) != 0) {
			topo_mod_dprintf(mod, "Failed to register propmethod "
			    "on %s=%" PRIx64 " (%s)", nodename, nodeinst,
			    topo_strerror(err));
			goto err;
		}
		if (topo_prop_setnonvolatile(tn, pgname,
		    TOPO_PROP_INITIATOR_DEV_FMRI, &err) != 0) {
			topo_mod_dprintf(mod, "Failed to nonvolatile flag for "
			    "prop %s/%s", pgname, TOPO_PROP_INITIATOR_DEV_FMRI);
			goto err;
		}
	} else if (strcmp(pgname, TOPO_PGROUP_SASPORT) == 0) {
		const char *errprops[] = {
		    TOPO_PROP_SASPORT_INV_DWORD,
		    TOPO_PROP_SASPORT_RUN_DISP,
		    TOPO_PROP_SASPORT_LOSS_SYNC,
		    TOPO_PROP_SASPORT_RESET_PROB
		};
		const char *rateprops[] = {
		    TOPO_PROP_SASPORT_MAX_RATE,
		    TOPO_PROP_SASPORT_PROG_RATE,
		    TOPO_PROP_SASPORT_NEG_RATE
		};
		nvlist_t *arg_nvl = NULL;

		for (uint_t i = 0;
		    i < sizeof (errprops) / sizeof (errprops[0]);
		    i++) {
			fnvlist_add_string(nvl, "pname", errprops[i]);

			if (topo_prop_method_register(tn, pgname, errprops[i],
			    TOPO_TYPE_UINT64_ARRAY, TOPO_METH_SAS_PHY_ERR, nvl,
			    &err) != 0) {
				topo_mod_dprintf(mod, "Failed to set "
				    "up prop cb on %s=%" PRIx64 " (%s)",
				    nodename, nodeinst, topo_strerror(err));
				goto err;
			}
		}
		if (topo_mod_nvalloc(mod, &arg_nvl,  NV_UNIQUE_NAME) != 0) {
			(void) topo_mod_seterrno(mod, EMOD_NOMEM);
			goto err;
		}
		for (uint_t i = 0;
		    i < sizeof (rateprops) / sizeof (rateprops[0]);
		    i++) {
			fnvlist_add_string(arg_nvl, "pname", rateprops[i]);

			if (topo_prop_method_register(tn, pgname, rateprops[i],
			    TOPO_TYPE_UINT32_ARRAY, TOPO_METH_SAS_LINK_RATE,
			    arg_nvl, &err) != 0) {
				topo_mod_dprintf(mod, "Failed to set "
				    "up prop cb on %s=%" PRIx64 " (%s)",
				    nodename, nodeinst, topo_strerror(err));
				nvlist_free(arg_nvl);
				goto err;
			}
		}
		nvlist_free(arg_nvl);

	}
	ret = 0;
err:
	nvlist_free(nvl);
	return (ret);
}


static topo_vertex_t *
sas_create_vertex(topo_mod_t *mod, const char *name, topo_instance_t inst,
    struct sas_phy_info *phyinfo)
{
	topo_vertex_t *vtx;
	tnode_t *tn;
	topo_pgroup_info_t pgi;
	int err;
	nvlist_t *auth = NULL, *fmri = NULL;

	pgi.tpi_namestab = TOPO_STABILITY_PRIVATE;
	pgi.tpi_datastab = TOPO_STABILITY_PRIVATE;
	pgi.tpi_version = TOPO_VERSION;
	if (strcmp(name, TOPO_VTX_EXPANDER) == 0)
		pgi.tpi_name = TOPO_PGROUP_EXPANDER;
	else if (strcmp(name, TOPO_VTX_INITIATOR) == 0)
		pgi.tpi_name = TOPO_PGROUP_INITIATOR;
	else if (strcmp(name, TOPO_VTX_PORT) == 0)
		pgi.tpi_name = TOPO_PGROUP_SASPORT;
	else if (strcmp(name, TOPO_VTX_TARGET) == 0)
		pgi.tpi_name = TOPO_PGROUP_TARGET;
	else {
		topo_mod_dprintf(mod, "invalid vertex name: %s", name);
		return (NULL);
	}

	if ((vtx = topo_vertex_new(mod, name, inst)) == NULL) {
		/* errno set */
		topo_mod_dprintf(mod, "failed to create vertex: "
		    "%s=%" PRIx64 "", name, inst);
		return (NULL);
	}
	tn = topo_vertex_node(vtx);

	if (topo_mod_nvalloc(mod, &auth, NV_UNIQUE_NAME) != 0 ||
	    nvlist_add_string(auth, FM_FMRI_SAS_TYPE,
	    FM_FMRI_SAS_TYPE_PATHNODE) != 0) {
		(void) topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}
	if (strcmp(name, TOPO_VTX_PORT) == 0 && phyinfo != NULL) {
		/*
		 * if (phyinfo == NULL) {
		 *	goto err;
		 * }
		 */
		if (nvlist_add_uint32(auth, FM_FMRI_SAS_START_PHY,
		    phyinfo->start_phy) != 0 ||
		    nvlist_add_uint32(auth, FM_FMRI_SAS_END_PHY,
		    phyinfo->end_phy) != 0) {
			(void) topo_mod_seterrno(mod, EMOD_NOMEM);
			topo_mod_dprintf(mod, "failed to construct auth for "
			    "node: %s=%" PRIx64, name, inst);
			goto err;
		}
	}
	if ((fmri = topo_mod_sasfmri(mod, FM_SAS_SCHEME_VERSION, name, inst,
	    auth)) == NULL) {
		/* errno set */
		topo_mod_dprintf(mod, "failed to construct FMRI for "
		    "%s=%" PRIx64 ": %s", name, inst, topo_strerror(err));
		goto err;
	}
	nvlist_free(auth);

	if (topo_pgroup_create(tn, &pgi, &err) != 0) {
		(void) topo_mod_seterrno(mod, err);
		topo_mod_dprintf(mod, "failed to create %s propgroup on "
		    "%s=%" PRIx64 ": %s", pgi.tpi_name, name, inst,
		    topo_strerror(err));
		goto err;
	}
	if (topo_pgroup_create(tn, &protocol_pgroup, &err) < 0 ||
	    topo_prop_set_fmri(tn, TOPO_PGROUP_PROTOCOL, TOPO_PROP_RESOURCE,
	    TOPO_PROP_IMMUTABLE, fmri, &err) < 0) {
		(void) topo_mod_seterrno(mod, err);
		topo_mod_dprintf(mod, "failed to create %s propgroup on "
		    "%s=%" PRIx64 ": %s", TOPO_PGROUP_PROTOCOL, name, inst,
		    topo_strerror(err));
		goto err;
	}
	nvlist_free(fmri);

	/*
	 * Make sure the appropriate methods to retrieve FMRIs and dynamic
	 * properties are configured for each node that corresponds to a
	 * hardware component.
	 */
	if (sas_prop_method_register(mod, tn, pgi.tpi_name) != 0) {
		topo_mod_dprintf(mod, "failed to register property "
		    "methods for %s=%" PRIx64, name, inst);
		goto err;
	}

	return (vtx);
err:
	nvlist_free(auth);
	nvlist_free(fmri);
	topo_vertex_destroy(mod, vtx);
	return (NULL);
}

static uint64_t
wwn_to_uint64(HBA_WWN wwn)
{
	uint64_t res;
	(void) memcpy(&res, &wwn, sizeof (uint64_t));
	return (ntohll(res));
}

typedef struct sas_port {
	topo_list_t		sp_list;
	uint64_t		sp_att_wwn;
	topo_vertex_t		*sp_vtx; /* port pointer */
	boolean_t		sp_is_expander;
	boolean_t		sp_has_hba_connection;
} sas_port_t;

typedef struct sas_vtx_search {
	topo_instance_t	inst;
	const char	*name;
	topo_list_t	*result_list;
} sas_vtx_search_t;

typedef struct sas_vtx {
	topo_list_t	tds_list;
	topo_vertex_t	*tds_vtx;
} sas_vtx_t;

/* Finds vertices matching the given tn_instance and tn_name. */
int
sas_vtx_match(topo_hdl_t *thp, topo_vertex_t *vtx, boolean_t last,
    void *arg)
{
	sas_vtx_search_t *search = arg;
	sas_vtx_t *res = NULL;
	tnode_t *node = topo_vertex_node(vtx);

	if (node->tn_instance == search->inst &&
	    strcmp(node->tn_name, search->name) == 0) {
		res = topo_hdl_zalloc(thp, sizeof (sas_vtx_t));
		if (res != NULL) {
			res->tds_vtx = vtx;
			topo_list_append(search->result_list, res);
		}
	}
	return (TOPO_WALK_NEXT);
}

/*
 * Search through this mod's digraph for either a connection between two nodes
 * or a specific node. This is an abstraction over the sas_vtx_match routine.
 *
 * The search_wwn argument is required.
 *
 * If the caller passes in an expander vtx_type then this function returns the
 * expander vertex that matches the search_wwn.
 *
 * If the caller passes in a port vtx_type and an att_wwn value then this
 * searches for a linkage between search_wwn and att_wwn. Any ports in the
 * digraph that have a local WWN value of search_wwn and are connected to
 * an att_wwn port are added to a list and returned.
 */
static uint_t
sas_find_connected_vtx(topo_mod_t *mod, uint64_t att_wwn, uint64_t search_wwn,
    const char *vtx_type, topo_list_t *res_list)
{
	topo_list_t *vtx_list = topo_mod_zalloc(mod, sizeof (topo_list_t));
	sas_vtx_search_t search;
	search.inst = search_wwn;
	search.name = vtx_type;
	search.result_list = vtx_list;
	sas_vtx_t *res;

	uint_t nfound = 0;

	(void) topo_vertex_iter(mod->tm_hdl, topo_digraph_get(mod->tm_hdl,
	    FM_FMRI_SCHEME_SAS), sas_vtx_match, &search);

	for (sas_vtx_t *res = topo_list_next(vtx_list);
	    res != NULL; res = topo_list_next(res)) {
		if (strcmp(vtx_type, TOPO_VTX_PORT) == 0 && att_wwn != 0) {
			/* The caller is looking for a specific linkage. */
			sas_port_t *res_port = topo_node_getspecific(
			    topo_vertex_node(res->tds_vtx));

			if ((res_port != NULL &&
			    res_port->sp_att_wwn != att_wwn) ||
			    res->tds_vtx->tvt_nincoming != 0)
				continue;

			sas_vtx_t *vtx = topo_mod_zalloc(
			    mod, sizeof (sas_vtx_t));
			vtx->tds_vtx = res->tds_vtx;
			topo_list_append(res_list, vtx);
			nfound++;
		} else if (strcmp(vtx_type, TOPO_VTX_EXPANDER) == 0) {
			/*
			 * The caller is looking for anything that matches
			 * search_wwn. There should only be one expander vtx
			 * matching this description.
			 */
			sas_vtx_t *vtx = topo_mod_zalloc(
			    mod, sizeof (sas_vtx_t));
			vtx->tds_vtx = res->tds_vtx;
			topo_list_append(res_list, vtx);
			nfound++;
		}
	}

	/* Clean up all the garbage used by the search routine. */
	res = topo_list_next(vtx_list);
	while (res != NULL) {
		sas_vtx_t *tmp = res;

		res = topo_list_next(res);
		topo_mod_free(mod, tmp, sizeof (sas_vtx_t));
	}
	topo_mod_free(mod, vtx_list, sizeof (topo_list_t));

	return (nfound);
}

/*
 * Common routine to create wide ports for expander phys.
 *
 * This assumes that the att_wwn and local_wwn have the correct byte order
 * already.
 */
int
sas_wide_port_create(topo_mod_t *mod, const char *smp_path,
    struct sas_phy_info *wide_port_phys, uint64_t att_wwn, uint64_t local_wwn)
{
	tnode_t *tn;
	int ret = 0, err;
	sas_port_t *expd_port = NULL;

	if ((expd_port =
	    topo_mod_zalloc(mod, sizeof (sas_port_t))) == NULL) {
		ret = -1;
		goto done;
	}

	expd_port->sp_att_wwn = att_wwn;
	expd_port->sp_is_expander = B_TRUE;
	if ((expd_port->sp_vtx = sas_create_vertex(mod, TOPO_VTX_PORT,
	    local_wwn, wide_port_phys)) == NULL) {
		topo_mod_dprintf(mod,
		    "Failed to create vtx %s=%" PRIx64, TOPO_VTX_PORT,
		    local_wwn);
		topo_mod_free(mod, expd_port, sizeof (sas_port_t));
		ret = -1;
		goto done;
	}

	tn = topo_vertex_node(expd_port->sp_vtx);

	if (topo_prop_set_uint64(tn, TOPO_PGROUP_SASPORT,
	    TOPO_PROP_SASPORT_LOCAL_ADDR, TOPO_PROP_IMMUTABLE,
	    local_wwn, &err) != 0 ||
	    topo_prop_set_uint64(tn, TOPO_PGROUP_SASPORT,
	    TOPO_PROP_SASPORT_ATTACH_ADDR, TOPO_PROP_IMMUTABLE,
	    att_wwn, &err) != 0 ||
	    topo_prop_set_string(tn, TOPO_PGROUP_SASPORT,
	    TOPO_PROP_SASPORT_ANAME, TOPO_PROP_IMMUTABLE,
	    smp_path, &err) != 0 ||
	    topo_prop_set_string(tn, TOPO_PGROUP_SASPORT,
	    TOPO_PROP_SASPORT_TYPE, TOPO_PROP_IMMUTABLE,
	    TOPO_SASPORT_TYPE_EXPANDER, &err) != 0) {
		topo_mod_dprintf(mod,
		    "Failed to set props on %s=%" PRIx64 " (%s)",
		    topo_node_name(tn), topo_node_instance(tn),
		    topo_strerror(err));
		ret = -1;
		goto done;
	}
	topo_node_setspecific(tn, expd_port);

done:
	if (ret != 0 && expd_port != NULL) {
		topo_mod_free(mod, expd_port, sizeof (sas_port_t));
	}
	return (ret);
}

static int
sas_expander_discover(topo_mod_t *mod, const char *smp_path)
{
	int ret = 0;
	int i;
	uint8_t *smp_resp, num_phys;
	uint64_t expd_addr;
	size_t smp_resp_len;

	smp_target_def_t *tdef = NULL;
	smp_target_t *tgt = NULL;
	smp_action_t *axn = NULL;
	smp_report_general_resp_t *report_resp = NULL;
	smp_report_manufacturer_info_resp_t *manuf_resp = NULL;
	const char *nodename;
	topo_instance_t nodeinst;
	char exp_vid[9], exp_pid[17], *space;

	smp_function_t func;
	smp_result_t result;

	topo_vertex_t *expd_vtx = NULL;
	struct sas_phy_info phyinfo;
	sas_port_t *port_info = NULL;
	tnode_t *tn = NULL;
	int err;

	if ((tdef = topo_mod_zalloc(mod, sizeof (smp_target_def_t))) == NULL) {
		return (topo_mod_seterrno(mod, EMOD_NOMEM));
	}

	tdef->std_def = smp_path;

	if ((tgt = smp_open(tdef)) == NULL) {
		ret = -1;
		topo_mod_dprintf(mod, "failed to open SMP target\n");
		goto done;
	}

	func = SMP_FUNC_REPORT_GENERAL;
	axn = smp_action_alloc(func, tgt, 0);

	if (smp_exec(axn, tgt) != 0) {
		ret = -1;
		smp_action_free(axn);
		goto done;
	}

	smp_action_get_response(axn, &result, (void **)&smp_resp,
	    &smp_resp_len);
	smp_action_free(axn);

	if (result != SMP_RES_FUNCTION_ACCEPTED) {
		ret = -1;
		goto done;
	}

	report_resp = (smp_report_general_resp_t *)smp_resp;
	num_phys = report_resp->srgr_number_of_phys;
	expd_addr = ntohll(report_resp->srgr_enclosure_logical_identifier);

	phyinfo.start_phy = 0;
	phyinfo.end_phy = (phyinfo.start_phy + num_phys) - (num_phys - 1);
	if ((expd_vtx = sas_create_vertex(mod, TOPO_VTX_EXPANDER, expd_addr,
	    &phyinfo)) == NULL) {
		ret = -1;
		goto done;
	}
	port_info = topo_mod_zalloc(mod, sizeof (sas_port_t));
	port_info->sp_vtx = expd_vtx;
	port_info->sp_is_expander = B_TRUE;

	tn = topo_vertex_node(expd_vtx);
	topo_node_setspecific(tn, port_info);

	nodename = topo_node_name(tn);
	nodeinst = topo_node_instance(tn);

	if (topo_prop_set_string(tn, TOPO_PGROUP_EXPANDER,
	    TOPO_PROP_EXPANDER_DEVFS_PATH, TOPO_PROP_IMMUTABLE,
	    smp_path, &err) != 0) {
		topo_mod_dprintf(mod, "Failed to set props on %s=%" PRIx64,
		    nodename, nodeinst);
		ret = -1;
		goto done;
	}

	/*
	 * Send a REPORT MANUFACTURER INFO command to the expander to retrieve
	 * the manufacturer and model information.
	 *
	 * See the SPL section 9.4.4.4
	 */
	if ((axn = smp_action_alloc(SMP_FUNC_REPORT_MANUFACTURER_INFO, tgt,
	    0)) == NULL) {
		topo_mod_dprintf(mod, "%s: failed to allocate smp action (%s)",
		    __func__, smp_errmsg());
		ret = -1;
		goto done;
	}
	if (smp_exec(axn, tgt) != 0) {
		topo_mod_dprintf(mod, "%s: smp_exec of cmd 0x%x failed (%s)",
		    __func__, SMP_FUNC_REPORT_MANUFACTURER_INFO, smp_errmsg());
		smp_action_free(axn);
		ret = -1;
		goto done;
	}
	smp_action_get_response(axn, &result, (void **)&smp_resp,
	    &smp_resp_len);
	manuf_resp = (smp_report_manufacturer_info_resp_t *)smp_resp;
	smp_action_free(axn);

	/*
	 * SMP strings are padded with trailing whitespace and not
	 * NUL-terminated.  To deal with this, we copy the strings into buffers
	 * that are large enough to hold the max string + a NUL.  Then we look
	 * for the first space character in the copied string and, if found,
	 * set it to NUL.
	 */
	(void) strlcpy(exp_vid, manuf_resp->srmir_vendor_identification,
	    sizeof (exp_vid));
	(void) strlcpy(exp_pid, manuf_resp->srmir_product_identification,
	    sizeof (exp_pid));

	if ((space = strchr(exp_vid, ' ')) != NULL)
		*space = '\0';
	if ((space = strchr(exp_pid, ' ')) != NULL)
		*space = '\0';

	if (topo_prop_set_string(tn, TOPO_PGROUP_EXPANDER,
	    TOPO_PROP_EXPANDER_MANUF, TOPO_PROP_IMMUTABLE, exp_vid, &err) !=
	    0 ||
	    topo_prop_set_string(tn, TOPO_PGROUP_EXPANDER,
	    TOPO_PROP_EXPANDER_MODEL, TOPO_PROP_IMMUTABLE, exp_pid, &err) !=
	    0) {
		topo_mod_dprintf(mod, "%s: failed to set properties on %s=%"
		    PRIx64 " (%s)", __func__, nodename, nodeinst,
		    topo_strerror(err));
		ret = -1;
		goto done;
	}

	boolean_t wide_port_discovery = B_FALSE;
	uint64_t wide_port_att_wwn;
	struct sas_phy_info wide_port_phys;
	bzero(&wide_port_phys, sizeof (struct sas_phy_info));
	for (i = 0; i < num_phys; i++) {
		smp_discover_req_t *disc_req = NULL;
		smp_discover_resp_t *disc_resp = NULL;

		func = SMP_FUNC_DISCOVER;
		axn = smp_action_alloc(func, tgt, 0);
		smp_action_get_request(axn, (void **) &disc_req, NULL);
		disc_req->sdr_phy_identifier = i;

		if (smp_exec(axn, tgt) != 0) {
			topo_mod_dprintf(mod, "smp_exec failed\n");
			smp_action_free(axn);
			goto done;
		}

		smp_action_get_response(axn, &result, (void **) &smp_resp,
		    &smp_resp_len);
		smp_action_free(axn);

		disc_resp = (smp_discover_resp_t *)smp_resp;
		if (result != SMP_RES_FUNCTION_ACCEPTED &&
		    result != SMP_RES_PHY_VACANT) {
			topo_mod_dprintf(mod, "%s: error in SMP response (%d)",
			    __func__, result);
			goto done;
		}

		/* There's nothing at this phy, so ignore it and move on. */
		if (result == SMP_RES_PHY_VACANT) {
			continue;
		}

		if (disc_resp->sdr_attached_device_type == SMP_DEV_SAS_SATA &&
		    (disc_resp->sdr_attached_ssp_target ||
		    disc_resp->sdr_attached_stp_target) &&
		    disc_resp->sdr_connector_type >= SES_SASCONN_T_SFF_8482_R &&
		    disc_resp->sdr_connector_type <= SES_SASCONN_T_VIRTUAL &&
		    !disc_resp->sdr_attached_smp_target &&
		    !disc_resp->sdr_attached_smp_initiator) {
			/*
			 * Acceptable values (as of SES-3) for
			 * sdr_connector_type is 0x20 - 0x2F. These are the
			 * 'Internal connectors to end devices' according to
			 * the SES spec.
			 *
			 * XXX sdr_attached_smp_initiator is B_TRUE for SMP
			 * devices. We should map these too, but for now ignore
			 * them. They likely need their own (empty?) property
			 * group.
			 */

			/*
			 * The current phy cannot be part of a wide
			 * port, so the previous wide port discovery
			 * effort must be committed.
			 */
			if (wide_port_discovery) {
				wide_port_discovery = B_FALSE;
				if (sas_wide_port_create(mod, smp_path,
				    &wide_port_phys,
				    htonll(wide_port_att_wwn),
				    expd_addr) != 0) {
					goto done;
				}
			}
			topo_vertex_t *ex_pt_vtx, *port_vtx, *tgt_vtx;

			/* Phy info for expander port is the expander's phy. */
			phyinfo.start_phy = disc_resp->sdr_phy_identifier;
			phyinfo.end_phy = disc_resp->sdr_phy_identifier;
			if ((ex_pt_vtx = sas_create_vertex(mod, TOPO_VTX_PORT,
			    ntohll(disc_resp->sdr_sas_addr),
			    &phyinfo)) == NULL) {
				ret = -1;
				goto done;
			}

			tn = topo_vertex_node(ex_pt_vtx);
			if (topo_prop_set_uint64(tn, TOPO_PGROUP_SASPORT,
			    TOPO_PROP_SASPORT_LOCAL_ADDR, TOPO_PROP_IMMUTABLE,
			    ntohll(disc_resp->sdr_sas_addr), &err) != 0 ||
			    topo_prop_set_uint64(tn, TOPO_PGROUP_SASPORT,
			    TOPO_PROP_SASPORT_ATTACH_ADDR, TOPO_PROP_IMMUTABLE,
			    ntohll(disc_resp->sdr_attached_sas_addr),
			    &err) != 0 ||
			    topo_prop_set_string(tn, TOPO_PGROUP_SASPORT,
			    TOPO_PROP_SASPORT_ANAME, TOPO_PROP_IMMUTABLE,
			    smp_path, &err) != 0 ||
			    topo_prop_set_string(tn, TOPO_PGROUP_SASPORT,
			    TOPO_PROP_SASPORT_TYPE, TOPO_PROP_IMMUTABLE,
			    TOPO_SASPORT_TYPE_EXPANDER, &err) != 0) {
				topo_mod_dprintf(mod, "Failed to set props on "
				    "%s=%" PRIx64, nodename, nodeinst);
				ret = -1;
				goto done;
			}

			if (topo_edge_new(mod, expd_vtx, ex_pt_vtx) != 0) {
				topo_vertex_destroy(mod, ex_pt_vtx);
				ret = -1;
				goto done;
			}

			/*
			 * Phy info for attached device port is the device's
			 * internal phy.
			 */
			phyinfo.start_phy =
			    disc_resp->sdr_attached_phy_identifier;
			phyinfo.end_phy =
			    disc_resp->sdr_attached_phy_identifier;
			if ((port_vtx = sas_create_vertex(mod, TOPO_VTX_PORT,
			    ntohll(disc_resp->sdr_attached_sas_addr),
			    &phyinfo)) == NULL) {
				topo_vertex_destroy(mod, ex_pt_vtx);
				ret = -1;
				goto done;
			}

			tn = topo_vertex_node(port_vtx);
			if (topo_prop_set_uint64(tn, TOPO_PGROUP_SASPORT,
			    TOPO_PROP_SASPORT_LOCAL_ADDR, TOPO_PROP_IMMUTABLE,
			    ntohll(disc_resp->sdr_attached_sas_addr), &err)
			    != 0 ||
			    topo_prop_set_uint64(tn, TOPO_PGROUP_SASPORT,
			    TOPO_PROP_SASPORT_ATTACH_ADDR, TOPO_PROP_IMMUTABLE,
			    ntohll(disc_resp->sdr_sas_addr), &err) != 0 ||
			    topo_prop_set_string(tn, TOPO_PGROUP_SASPORT,
			    TOPO_PROP_SASPORT_TYPE, TOPO_PROP_IMMUTABLE,
			    TOPO_SASPORT_TYPE_TARGET, &err) != 0 ||
			    topo_prop_set_string(tn, TOPO_PGROUP_SASPORT,
			    TOPO_PROP_SASPORT_ANAME, TOPO_PROP_IMMUTABLE, "TBD",
			    &err) != 0) {
				topo_mod_dprintf(mod, "Failed to set props on "
				    "%s=%" PRIx64 " (%s)", nodename, nodeinst,
				    topo_strerror(err));
				ret = -1;
				goto done;
			}

			if (topo_edge_new(mod, ex_pt_vtx, port_vtx) != 0) {
				topo_vertex_destroy(mod, ex_pt_vtx);
				topo_vertex_destroy(mod, port_vtx);
				ret = -1;
				goto done;
			}

			/* This is a target disk. */
			if ((tgt_vtx = sas_create_vertex(mod, TOPO_VTX_TARGET,
			    ntohll(disc_resp->sdr_attached_device_name),
			    &phyinfo)) == NULL) {
				topo_vertex_destroy(mod, ex_pt_vtx);
				topo_vertex_destroy(mod, port_vtx);
				ret = -1;
				goto done;
			}

			if (topo_edge_new(mod, port_vtx, tgt_vtx) != 0) {
				topo_vertex_destroy(mod, ex_pt_vtx);
				topo_vertex_destroy(mod, port_vtx);
				topo_vertex_destroy(mod, tgt_vtx);
				ret = -1;
				goto done;
			}

			tn = topo_vertex_node(tgt_vtx);

		} else if (disc_resp->sdr_attached_device_type
		    == SMP_DEV_EXPANDER ||
		    (disc_resp->sdr_attached_ssp_initiator ||
		    disc_resp->sdr_attached_stp_initiator ||
		    disc_resp->sdr_attached_smp_initiator)) {
			/*
			 * This phy is for another 'complicated' device like an
			 * expander or an HBA. This phy may be in a wide port
			 * configuration.
			 *
			 * To discover wide ports we allow the phy discovery
			 * loop to continue to run. When this block
			 * first encounters a possibly wide port it sets the
			 * start phy to the current phy, and it is not modified
			 * again.
			 *
			 * Each time this block finds the same attached SAS
			 * address we update the end phy identifier to be the
			 * current phy.
			 *
			 * Once the phy discovery loop finds a new attached SAS
			 * address we know that the (possibly) wide port is done
			 * being discovered and it should be 'committed.'
			 */

			/*
			 * The current phy cannot be part of a wide
			 * port, so the previous wide port discovery
			 * effort must be committed.
			 */
			if (disc_resp->sdr_attached_sas_addr
			    != wide_port_att_wwn && wide_port_discovery) {
				wide_port_discovery = B_FALSE;
				if (sas_wide_port_create(mod, smp_path,
				    &wide_port_phys,
				    htonll(wide_port_att_wwn),
				    expd_addr) != 0) {
					goto done;
				}
			}

			if (!wide_port_discovery) {
				/* New wide port discovery run. */
				wide_port_discovery = B_TRUE;
				wide_port_phys.start_phy =
				    disc_resp->sdr_phy_identifier;
				wide_port_att_wwn =
				    disc_resp->sdr_attached_sas_addr;
			}

			wide_port_phys.end_phy =
			    disc_resp->sdr_phy_identifier;
		}
	}

done:
	if (tgt != NULL)
		smp_close(tgt);
	topo_mod_free(mod, tdef, sizeof (smp_target_def_t));
	return (ret);
}

typedef struct sas_topo_iter {
	topo_mod_t	*sas_mod;
	uint64_t	sas_search_wwn;
} sas_topo_iter_t;

/* Responsible for creating links from HBA -> fanout expanders. */
static int
sas_connect_hba(topo_hdl_t *hdl, topo_edge_t *edge, boolean_t last, void* arg)
{
	sas_topo_iter_t *iter = arg;
	topo_mod_t *mod = iter->sas_mod;
	tnode_t *node = topo_vertex_node(edge->tve_vertex);
	sas_port_t *hba_port = topo_node_getspecific(node);
	topo_vertex_t *expd_port_vtx = NULL;
	topo_vertex_t *expd_vtx = NULL;
	sas_port_t *expd_port = NULL;
	sas_vtx_t *vtx;
	int ret = TOPO_WALK_NEXT;

	topo_list_t *vtx_list = topo_mod_zalloc(
	    mod, sizeof (topo_list_t));

	if (strcmp(node->tn_name, TOPO_VTX_PORT) == 0 &&
	    edge->tve_vertex->tvt_noutgoing == 0) {
		/*
		 * This is a port vtx that isn't connected to anything. We need
		 * to:
		 * - find the expander port that this hba port is connected to.
		 * - if not already connected, connect the expander port to the
		 *   expander itself.
		 */
		uint_t nfound = sas_find_connected_vtx(mod,
		    topo_node_instance(node), hba_port->sp_att_wwn,
		    TOPO_VTX_PORT, vtx_list);

		/*
		 * XXX need to match up the phys in case this expd is
		 * connected to more than one hba. In that case nfound should be
		 * > 1.
		 */
		if (nfound != 1) {
			topo_mod_dprintf(mod, "found unexpected number of "
			    "vertex connections from HBA %" PRIx64 " to "
			    "%" PRIx64 " (%d)", topo_node_instance(node),
			    hba_port->sp_att_wwn,
			    nfound);

			ret = TOPO_WALK_ERR;
			goto out;
		}
		sas_vtx_t *vtx = topo_list_next(vtx_list);
		expd_port_vtx = vtx->tds_vtx;
		if (expd_port_vtx == NULL ||
		    topo_edge_new(mod, edge->tve_vertex,
		    expd_port_vtx) != 0) {
			goto out;
		}
		topo_list_delete(vtx_list, vtx);
		topo_mod_free(mod, vtx, sizeof (sas_vtx_t));

		nfound = sas_find_connected_vtx(mod,
		    0, /* expd vtx doesn't have an attached SAS addr */
		    topo_node_instance(topo_vertex_node(expd_port_vtx)),
		    TOPO_VTX_EXPANDER, vtx_list);

		/* There should only be one expander vtx with this SAS addr. */
		if (nfound != 1) {
			topo_mod_dprintf(mod, "could not find vertex %" PRIx64
			    " (%d)",
			    topo_node_instance(topo_vertex_node(expd_port_vtx)),
			    nfound);

			ret = TOPO_WALK_ERR;
			goto out;
		}

		expd_vtx =
		    ((sas_vtx_t *)topo_list_next(vtx_list))->tds_vtx;
		if (expd_vtx == NULL ||
		    topo_edge_new(mod, expd_port_vtx,
		    expd_vtx) != 0) {
			goto out;
		}
		expd_port = topo_node_getspecific(topo_vertex_node(expd_vtx));
		expd_port->sp_has_hba_connection = B_TRUE;
	}

out:
	vtx = topo_list_next(vtx_list);
	while (vtx != NULL) {
		sas_vtx_t *tmp = vtx;

		vtx = topo_list_next(vtx);
		topo_mod_free(mod, tmp, sizeof (sas_vtx_t));
	}
	topo_mod_free(mod, vtx_list, sizeof (topo_list_t));

	return (ret);
}

static int
sas_expd_interconnect(topo_hdl_t *hdl, topo_vertex_t *vtx,
    sas_topo_iter_t *iter)
{
	int ret = 0;
	tnode_t *node = topo_vertex_node(vtx);
	topo_mod_t *mod = iter->sas_mod;
	topo_list_t *list = topo_mod_zalloc(
	    mod, sizeof (topo_list_t));
	sas_port_t *port = topo_node_getspecific(node);
	topo_vertex_t *port_vtx = NULL;
	sas_vtx_t *disc_vtx;

	uint_t nfound = sas_find_connected_vtx(mod, node->tn_instance,
	    port->sp_att_wwn, TOPO_VTX_PORT, list);

	if (nfound == 0) {
		ret = -1;
		goto out;
	}

	/*
	 * XXX make this work for multiple expd <-> expd connections. Likely
	 * need to compare local/att port phys.
	 */
	if ((port_vtx = ((sas_vtx_t *)topo_list_next(list))->tds_vtx) == NULL) {
		ret = -1;
		goto out;
	}

	if (topo_edge_new(mod, vtx, port_vtx) != 0) {
		goto out;
	}

out:
	disc_vtx = topo_list_next(list);
	while (disc_vtx) {
		sas_vtx_t *tmp = disc_vtx;

		disc_vtx = topo_list_next(disc_vtx);
		topo_mod_free(mod, tmp, sizeof (sas_vtx_t));
	}
	topo_mod_free(mod, list, sizeof (topo_list_t));

	return (ret);
}

/*
 * This routine is responsible for connecting expander port vertices to their
 * associated expander. The trick is getting the 'direction' of the connection
 * correct since SMP does not provide this information.
 */
static int
sas_connect_expd(topo_hdl_t *hdl, topo_vertex_t *vtx, sas_topo_iter_t *iter)
{
	int ret = TOPO_WALK_NEXT;
	tnode_t *node = topo_vertex_node(vtx);
	topo_mod_t *mod = iter->sas_mod;
	topo_vertex_t *expd_vtx = NULL;
	sas_port_t *disc_expd = NULL;
	sas_vtx_t *disc_vtx;

	topo_list_t *list = topo_mod_zalloc(
	    mod, sizeof (topo_list_t));

	/* Find the port's corresponding expander vertex. */
	uint_t nfound = sas_find_connected_vtx(mod, 0,
	    node->tn_instance, TOPO_VTX_EXPANDER, list);
	if (nfound == 0) {
		ret = TOPO_WALK_ERR;
		goto out;
	}

	if ((expd_vtx = ((sas_vtx_t *)topo_list_next(list))->tds_vtx) == NULL) {
		ret = TOPO_WALK_ERR;
		goto out;
	}

	disc_expd = topo_node_getspecific(topo_vertex_node(expd_vtx));
	/*
	 * XXX This assumes only one of the expanders is connected to an HBA.
	 * It should be possible for two expanders to both be connected to HBAs
	 * and also connected to each other. However, if we do this today the
	 * path finding logic in topo ends up doing infinite recursion trying
	 * to find targets.
	 */
	if (!disc_expd->sp_has_hba_connection) {
		if (topo_edge_new(mod, vtx, expd_vtx) != 0) {
			ret = TOPO_WALK_ERR;
			goto out;
		}
	} else {
		if (topo_edge_new(mod, expd_vtx, vtx) != 0) {
			ret = TOPO_WALK_ERR;
			goto out;
		}
	}

out:
	disc_vtx = topo_list_next(list);
	while (disc_vtx) {
		sas_vtx_t *tmp = disc_vtx;

		disc_vtx = topo_list_next(disc_vtx);
		topo_mod_free(mod, tmp, sizeof (sas_vtx_t));
	}
	topo_mod_free(mod, list, sizeof (topo_list_t));

	return (ret);
}

static int
sas_vtx_final_pass(topo_hdl_t *hdl, topo_vertex_t *vtx, boolean_t last,
    void *arg)
{
	sas_topo_iter_t *iter = arg;
	tnode_t *node = topo_vertex_node(vtx);
	sas_port_t *port = topo_node_getspecific(node);

	if (node != NULL && strcmp(topo_node_name(node), TOPO_VTX_PORT) == 0) {
		/*
		 * Connect this outbound port to another expander's inbound
		 * port.
		 */
		if (port != NULL && port->sp_vtx->tvt_noutgoing == 0 &&
		    port->sp_is_expander) {
			(void) sas_expd_interconnect(hdl, vtx, iter);
		}
	}

	return (0);
}

static int
sas_vtx_iter(topo_hdl_t *hdl, topo_vertex_t *vtx, boolean_t last, void *arg)
{
	sas_topo_iter_t *iter = arg;
	tnode_t *node = topo_vertex_node(vtx);
	sas_port_t *port = topo_node_getspecific(node);

	if (strcmp(topo_node_name(node), TOPO_VTX_INITIATOR) == 0) {
		return (topo_edge_iter(hdl, vtx, sas_connect_hba, iter));
	}

	if (strcmp(topo_node_name(node), TOPO_VTX_PORT) == 0) {
		/* Connect the port to its expander vtx. */
		if (port != NULL && port->sp_is_expander &&
		    port->sp_vtx->tvt_nincoming == 0)
			return (sas_connect_expd(hdl, vtx, iter));

	}
	return (TOPO_WALK_NEXT);
}

typedef struct sas_hba_enum {
	HBA_HANDLE handle;
	SMHBA_ADAPTERATTRIBUTES *ad_attrs;
	uint_t port;
	topo_vertex_t *initiator;
	topo_list_t *hba_list;
	char aname[256];
} sas_hba_enum_t;

static int
sas_enum_hba_port(topo_mod_t *mod, sas_hba_enum_t *hbadata)
{
	SMHBA_PORTATTRIBUTES *attrs = NULL;
	SMHBA_SAS_PORT *sas_port;
	SMHBA_SAS_PHY phy_attrs;
	HBA_UINT32 num_phys;
	uint64_t hba_wwn;
	struct sas_phy_info phyinfo;
	sas_port_t *sas_hba_port = NULL;
	tnode_t *tn;
	int err, ret;
	topo_vertex_t *hba_port = NULL, *dev_port = NULL;
	topo_vertex_t *dev = NULL;

	attrs = topo_mod_zalloc(mod, sizeof (SMHBA_PORTATTRIBUTES));
	sas_port = topo_mod_zalloc(mod, sizeof (SMHBA_SAS_PORT));
	attrs->PortSpecificAttribute.SASPort = sas_port;

	if ((ret = SMHBA_GetAdapterPortAttributes(hbadata->handle,
	    hbadata->port, attrs)) != HBA_STATUS_OK) {
		goto err;
	}
	hba_wwn = wwn_to_uint64(sas_port->LocalSASAddress);
	num_phys = sas_port->NumberofPhys;

	/*
	 * Only create one logical initiator vertex for all of the HBA ports.
	 */
	/*
	 * phyinfo.start_phy = 0;
	 * phyinfo.end_phy = num_phys - 1;
	 */
	if (hbadata->initiator == NULL) {
		if ((hbadata->initiator = sas_create_vertex(mod,
		    TOPO_VTX_INITIATOR, hba_wwn, NULL)) == NULL) {
			goto err;
		}

		/*
		 * Set the devfs path for this initiator so we can use it to
		 * correlate with corresponding hc topo node later.
		 *
		 * The info we get from libsmhbaapi w.r.t. manufacturer and
		 * model are specific to the board manufacturing info, but
		 * what we want is the product-specific info as derived from
		 * the PCI VID/PID.  So we will grab those from the
		 * corresponding node from the hc-scheme tree.
		 */
		tn = topo_vertex_node(hbadata->initiator);
		if (topo_prop_set_string(tn, TOPO_PGROUP_INITIATOR,
		    TOPO_PROP_INITIATOR_DEVFS_PATH, TOPO_PROP_IMMUTABLE,
		    hbadata->ad_attrs->HBASymbolicName, &err) != 0) {
			goto err;
		}
	}

	/* Calculate the beginning and end phys for this port */
	for (uint_t phy = 0; phy < num_phys; phy++) {
		if ((ret = SMHBA_GetSASPhyAttributes(hbadata->handle,
		    hbadata->port, phy, &phy_attrs)) != HBA_STATUS_OK) {
			topo_mod_free(mod, attrs,
			    sizeof (SMHBA_PORTATTRIBUTES));
			topo_mod_free(mod, sas_port,
			    sizeof (SMHBA_SAS_PORT));
			goto err;
		}

		if (phy == 0) {
			phyinfo.start_phy =  phy_attrs.PhyIdentifier;
		}
		phyinfo.end_phy = phy_attrs.PhyIdentifier;
	}

	if ((hba_port = sas_create_vertex(mod, TOPO_VTX_PORT, hba_wwn,
	    &phyinfo)) == NULL) {
		goto err;
	}

	tn = topo_vertex_node(hba_port);

	if (topo_prop_set_uint64(tn, TOPO_PGROUP_SASPORT,
	    TOPO_PROP_SASPORT_LOCAL_ADDR, TOPO_PROP_IMMUTABLE,
	    hba_wwn, &err) != 0 ||
	    topo_prop_set_uint64(tn, TOPO_PGROUP_SASPORT,
	    TOPO_PROP_SASPORT_ATTACH_ADDR, TOPO_PROP_IMMUTABLE,
	    wwn_to_uint64(sas_port->AttachedSASAddress), &err) != 0 ||
	    topo_prop_set_string(tn, TOPO_PGROUP_SASPORT,
	    TOPO_PROP_SASPORT_ANAME, TOPO_PROP_IMMUTABLE, hbadata->aname,
	    &err) != 0 ||
	    topo_prop_set_uint32(tn, TOPO_PGROUP_SASPORT,
	    TOPO_PROP_SASPORT_APORT, TOPO_PROP_IMMUTABLE, hbadata->port,
	    &err) != 0 ||
	    topo_prop_set_string(tn, TOPO_PGROUP_SASPORT,
	    TOPO_PROP_SASPORT_TYPE, TOPO_PROP_IMMUTABLE,
	    TOPO_SASPORT_TYPE_INITIATOR, &err)) {

		topo_mod_dprintf(mod, "Failed to set props on %s=%" PRIx64
		    " (%s)", topo_node_name(tn), topo_node_instance(tn),
		    topo_strerror(err));
		goto err;
	}

	/*
	 * Record that we created a unique port for this HBA.
	 * This will be referenced later if there are expanders in the
	 * topology.
	 */
	sas_hba_port = topo_mod_zalloc(mod, sizeof (sas_port_t));
	sas_hba_port->sp_att_wwn = wwn_to_uint64(
	    sas_port->AttachedSASAddress);
	sas_hba_port->sp_vtx = hba_port;

	topo_list_append(hbadata->hba_list, sas_hba_port);
	topo_node_setspecific(tn, sas_hba_port);

	if (topo_edge_new(mod, hbadata->initiator, hba_port) != 0) {
		goto err;
	}

	if (attrs->PortType == HBA_PORTTYPE_SASDEVICE) {
		/*
		 * Discovered a SAS or STP device connected directly to the
		 * HBA. This can sometimes include expander devices.
		 */
		if (sas_port->NumberofDiscoveredPorts > 1) {
			goto done;
		}

		/*
		 * SMHBAAPI doesn't give us attached device phy information.
		 * For HBA_PORTTYPE_SASDEVICE only phy 0 will be in use, unless
		 * there are virtual phys.
		 */
		phyinfo.start_phy = 0;
		phyinfo.end_phy = 0;
		if ((dev_port = sas_create_vertex(mod, TOPO_VTX_PORT,
		    wwn_to_uint64(sas_port->AttachedSASAddress),
		    &phyinfo)) == NULL) {
			goto err;
		}

		tn = topo_vertex_node(dev_port);
		if (topo_prop_set_uint64(tn, TOPO_PGROUP_SASPORT,
		    TOPO_PROP_SASPORT_LOCAL_ADDR, TOPO_PROP_IMMUTABLE,
		    wwn_to_uint64(sas_port->AttachedSASAddress), &err) != 0 ||
		    topo_prop_set_uint64(tn, TOPO_PGROUP_SASPORT,
		    TOPO_PROP_SASPORT_ATTACH_ADDR, TOPO_PROP_IMMUTABLE,
		    hba_wwn, &err) != 0 ||
		    topo_prop_set_string(tn, TOPO_PGROUP_SASPORT,
		    TOPO_PROP_SASPORT_TYPE, TOPO_PROP_IMMUTABLE,
		    TOPO_SASPORT_TYPE_TARGET, &err) != 0 ||
		    topo_prop_set_string(tn, TOPO_PGROUP_SASPORT,
		    TOPO_PROP_SASPORT_ANAME, TOPO_PROP_IMMUTABLE, "TBD",
		    &err) != 0) {

			topo_mod_dprintf(mod, "Failed to set props on %s=%"
			    PRIx64 " (%s)", topo_node_name(tn),
			    topo_node_instance(tn), topo_strerror(err));
			goto err;
		}

		if ((dev = sas_create_vertex(mod, TOPO_VTX_TARGET,
		    wwn_to_uint64(sas_port->AttachedSASAddress), &phyinfo))
		    == NULL) {
			goto err;
		}

		tn = topo_vertex_node(dev);
		if (topo_edge_new(mod, hba_port, dev_port) != 0 ||
		    topo_edge_new(mod, dev_port, dev) != 0) {
			goto err;
		}
	} else { /* Expanders? */
		goto done;
	}
done:
	topo_mod_free(mod, attrs, sizeof (SMHBA_PORTATTRIBUTES));
	topo_mod_free(mod, sas_port, sizeof (SMHBA_SAS_PORT));
	return (0);

err:
	topo_mod_free(mod, attrs, sizeof (SMHBA_PORTATTRIBUTES));
	topo_mod_free(mod, sas_port, sizeof (SMHBA_SAS_PORT));

	if (hbadata->initiator != NULL)
		topo_vertex_destroy(mod, hbadata->initiator);
	if (hba_port != NULL)
		topo_vertex_destroy(mod, hba_port);
	if (dev_port != NULL)
		topo_vertex_destroy(mod, dev_port);
	if (dev != NULL)
		topo_vertex_destroy(mod, dev);
	return (-1);
}

static int
sas_enum(topo_mod_t *mod, tnode_t *rnode, const char *name,
    topo_instance_t min, topo_instance_t max, void *notused1, void *notused2)
{
	if (topo_method_register(mod, rnode, sas_root_methods) != 0) {
		topo_mod_dprintf(mod, "failed to register scheme methods");
		/* errno set */
		return (-1);
	}

	if (getenv("TOPO_SASNOENUM"))
		return (0);

	int ret = -1;

	di_node_t root, smp;
	const char *smp_path = NULL;
	sas_port_t *hba_port;
	topo_list_t *hba_list = topo_mod_zalloc(mod, sizeof (topo_list_t));

	if (hba_list == NULL) {
		goto done;
	}

	/* Begin by discovering all HBAs and their immediate ports. */
	HBA_HANDLE handle;
	SMHBA_ADAPTERATTRIBUTES ad_attrs;
	HBA_UINT32 num_ports, num_adapters;

	num_adapters = HBA_GetNumberOfAdapters();
	if (num_adapters == 0) {
		ret = 0;
		goto done;
	}

	for (uint_t i = 0; i < num_adapters; i++) {
		sas_hba_enum_t hbadata = { 0 };

		if ((ret = HBA_GetAdapterName(i, hbadata.aname)) != 0) {
			topo_mod_dprintf(mod, "failed to get adapter name\n");
			goto done;
		}

		if ((handle = HBA_OpenAdapter(hbadata.aname)) == 0) {
			topo_mod_dprintf(mod, "failed to open adapter\n");
			goto done;
		}

		if ((ret = SMHBA_GetAdapterAttributes(handle, &ad_attrs)) !=
		    HBA_STATUS_OK) {
			topo_mod_dprintf(mod, "failed to get adapter attrs\n");
			HBA_CloseAdapter(handle);
			goto done;
		}

		if ((ret = SMHBA_GetNumberOfPorts(handle, &num_ports)) !=
		    HBA_STATUS_OK) {
			topo_mod_dprintf(mod, "failed to get num ports\n");
			HBA_CloseAdapter(handle);
			goto done;
		}

		hbadata.handle = handle;
		hbadata.ad_attrs = &ad_attrs;
		hbadata.hba_list = hba_list;
		for (uint_t port = 0; port < num_ports; port++) {
			hbadata.port = port;
			if (sas_enum_hba_port(mod, &hbadata) != 0) {
				HBA_CloseAdapter(handle);
				goto done;
			}
		}
		HBA_CloseAdapter(handle);
	}

	/* Iterate through the expanders in /dev/smp. */
	/* XXX why does topo_mod_devinfo() return ENOENT? */
	root = di_init("/", DINFOCPYALL);
	if (root == DI_NODE_NIL) {
		topo_mod_dprintf(mod, "di_init failed %s\n", strerror(errno));
		goto done;
	}

	for (smp = di_drv_first_node("smp", root);
	    smp != DI_NODE_NIL;
	    smp = di_drv_next_node(smp)) {
		char *full_smp_path;

		smp_path = di_devfs_path(smp);
		full_smp_path = topo_mod_zalloc(
		    mod, strlen(smp_path) + strlen("/devices:smp"));
		(void) sprintf(full_smp_path, "/devices%s:smp", smp_path);

		if (sas_expander_discover(mod, full_smp_path) != 0) {
			topo_mod_dprintf(mod, "expander discovery failed\n");
			goto done;
		}
	}

	sas_topo_iter_t iter;
	iter.sas_mod = mod;
	if (topo_vertex_iter(mod->tm_hdl,
	    topo_digraph_get(mod->tm_hdl, FM_FMRI_SCHEME_SAS),
	    sas_vtx_iter, &iter) != 0) {
		topo_mod_dprintf(mod, "failed to create links between HBAs and"
		    " expanders");
		ret = -1;
		goto done;
	}

	if (topo_vertex_iter(mod->tm_hdl,
	    topo_digraph_get(mod->tm_hdl, FM_FMRI_SCHEME_SAS),
	    sas_vtx_final_pass, &iter) != 0) {
		topo_mod_dprintf(mod, "failed to create links between"
		    " expanders");
		ret = -1;
		goto done;
	}

	ret = 0;
done:
	hba_port = topo_list_next(hba_list);
	while (hba_port != NULL) {
		sas_port_t *tmp = hba_port;

		hba_port = topo_list_next(hba_port);
		topo_mod_free(mod, tmp, sizeof (sas_port_t));
	}
	topo_mod_free(mod, hba_list, sizeof (topo_list_t));

	return (ret);
}

static void
sas_release(topo_mod_t *mod, tnode_t *node)
{
	topo_method_unregister_all(mod, node);
}
