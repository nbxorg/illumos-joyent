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
 * Copyright 2020 Joyent, Inc.
 */

/*
 * XXX add comment
 */

#include <libtopo.h>
#include <sys/fm/protocol.h>

#include <topo_digraph.h>
#define	__STDC_FORMAT_MACROS
#include <inttypes.h>

topo_digraph_t *
topo_digraph_get(topo_hdl_t *thp, const char *scheme)
{
	topo_digraph_t *tdg;

	for (tdg = topo_list_next(&thp->th_digraphs); tdg != NULL;
	    tdg = topo_list_next(tdg)) {
		if (strcmp(scheme, tdg->tdg_scheme) == 0)
			return (tdg);
	}
	return (NULL);
}

static topo_digraph_t *
find_digraph(topo_mod_t *mod)
{
	return (topo_digraph_get(mod->tm_hdl, mod->tm_info->tmi_scheme));
}

topo_digraph_t *
topo_digraph_new(topo_hdl_t *thp, topo_mod_t *mod, const char *scheme)
{
	topo_digraph_t *tdg;
	tnode_t *tn = NULL;

	if ((tdg = topo_mod_zalloc(mod, sizeof (topo_digraph_t))) == NULL) {
		(void) topo_hdl_seterrno(thp, ETOPO_NOMEM);
		return (NULL);
	}

	tdg->tdg_mod = mod;

	if ((tdg->tdg_scheme = topo_mod_strdup(mod, scheme)) == NULL) {
		(void) topo_hdl_seterrno(thp, ETOPO_NOMEM);
		goto err;
	}

	/*
	 * For digraph topologies, the "root" node, which gets passed in to
	 * the scheme module's enum method is not part of the actual graph
	 * structure per-se.
	 * Its purpose is simply to provide a place on which to register the
	 * scheme-specific methods.  Client code then invokes these methods via
	 * the topo_fmri_* interfaces.
	 */
	if ((tn = topo_mod_zalloc(mod, sizeof (tnode_t))) == NULL)
		goto err;

	tn->tn_state = TOPO_NODE_ROOT | TOPO_NODE_INIT;
	tn->tn_name = (char *)scheme;
	tn->tn_instance = 0;
	tn->tn_enum = mod;
	tn->tn_hdl = thp;
	topo_node_hold(tn);

	tdg->tdg_rootnode = tn;

	(void) pthread_mutex_init(&tdg->tdg_lock, NULL);

	return (tdg);
err:
	topo_mod_free(mod, tdg, sizeof (topo_digraph_t));
	return (NULL);
}

void
topo_digraph_destroy(topo_digraph_t *tdg)
{
	topo_mod_t *mod;

	if (tdg == NULL)
		return;

	mod = tdg->tdg_mod;
	(void) pthread_mutex_destroy(&tdg->tdg_lock);
	topo_mod_strfree(mod, (char *)tdg->tdg_scheme);
	topo_mod_free(mod, tdg->tdg_rootnode, sizeof (tnode_t));
	topo_mod_free(mod, tdg, sizeof (topo_digraph_t));
}

topo_vertex_t *
topo_vertex_new(topo_mod_t *mod, const char *name, topo_instance_t inst)
{
	tnode_t *tn = NULL;
	topo_vertex_t *vtx = NULL;
	topo_digraph_t *tdg;

	topo_mod_dprintf(mod, "Creating vertex %s=%" PRIx64 "", name, inst);
	if ((tdg = find_digraph(mod)) == NULL) {
		return (NULL);
	}
	if ((vtx = topo_mod_zalloc(mod, sizeof (topo_vertex_t))) == NULL ||
	    (tn = topo_mod_zalloc(mod, sizeof (tnode_t))) == NULL) {
		(void) topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}
	if ((tn->tn_name = topo_mod_strdup(mod, name)) == NULL) {
		(void) topo_mod_seterrno(mod, EMOD_NOMEM);
		goto err;
	}
	tn->tn_enum = mod;
	tn->tn_hdl = mod->tm_hdl;
	tn->tn_vtx = vtx;
	tn->tn_instance = inst;
	/*
	 * Adding the TOPO_NODE_ROOT state to the node has the effect of
	 * preventing topo_node_destroy() from trying to clean up the parent
	 * node's node hash, which is only necessary in tree topologies.
	 */
	tn->tn_state = TOPO_NODE_ROOT | TOPO_NODE_BOUND;
	vtx->tvt_node = tn;
	topo_node_hold(tn);

	/* Bump the refcnt on the module that's creating this vertex. */
	topo_mod_hold(mod);

	(void) pthread_mutex_lock(&tdg->tdg_lock);
	topo_list_append(&tdg->tdg_vertices, vtx);
	tdg->tdg_nvertices++;
	(void) pthread_mutex_unlock(&tdg->tdg_lock);

	return (vtx);
err:
	topo_mod_dprintf(mod, "failed to add create vertex %s=%" PRIx64 "(%s)",
	    name, inst, topo_strerror(topo_mod_errno(mod)));
	topo_mod_free(mod, tn, sizeof (tnode_t));
	topo_mod_free(mod, vtx, sizeof (topo_vertex_t));
	return (NULL);
}

tnode_t *
topo_vertex_node(topo_vertex_t *vtx)
{
	return (vtx->tvt_node);
}

/*
 * Convenience interface for deallocating a topo_vertex_t
 */
void
topo_vertex_destroy(topo_mod_t *mod, topo_vertex_t *vtx)
{
	topo_edge_t *edge;

	topo_node_unbind(vtx->tvt_node);

	edge = topo_list_next(&vtx->tvt_incoming);
	while (edge != NULL) {
		topo_edge_t *tmp = edge;

		edge = topo_list_next(edge);
		topo_mod_free(mod, tmp, sizeof (topo_edge_t));
	}

	edge = topo_list_next(&vtx->tvt_outgoing);
	while (edge != NULL) {
		topo_edge_t *tmp = edge;

		edge = topo_list_next(edge);
		topo_mod_free(mod, tmp, sizeof (topo_edge_t));
	}

	topo_mod_free(mod, vtx, sizeof (topo_vertex_t));
}

int
topo_vertex_iter(topo_hdl_t *thp, topo_digraph_t *tdg,
    int (*func)(topo_hdl_t *, topo_vertex_t *, boolean_t, void *), void *arg)
{
	uint_t n = 0;

	for (topo_vertex_t *vtx = topo_list_next(&tdg->tdg_vertices);
	    vtx != NULL; vtx = topo_list_next(vtx), n++) {
		int ret;
		boolean_t last_vtx = B_FALSE;

		if (n == (tdg->tdg_nvertices - 1))
			last_vtx = B_TRUE;

		ret = func(thp, vtx, last_vtx, arg);

		switch (ret) {
		case (TOPO_WALK_NEXT):
			continue;
		case (TOPO_WALK_TERMINATE):
			goto out;
		case (TOPO_WALK_ERR):
			/* FALLTHRU */
		default:
			return (-1);
		}
	}
out:
	return (0);
}

int
topo_edge_new(topo_mod_t *mod, topo_vertex_t *from, topo_vertex_t *to)
{
	topo_digraph_t *tdg;
	topo_edge_t *e_from = NULL, *e_to = NULL;

	topo_mod_dprintf(mod, "Adding edge from vertex %s=%" PRIx64 " to "
	    "%s=%" PRIx64"", topo_node_name(from->tvt_node),
	    topo_node_instance(from->tvt_node),
	    topo_node_name(to->tvt_node), topo_node_instance(to->tvt_node));

	if ((tdg = find_digraph(mod)) == NULL) {
		topo_mod_dprintf(mod, "Unable to find directed graph for "
		    "module %s", mod->tm_name);
		return (-1);
	}
	if ((e_from = topo_mod_zalloc(mod, sizeof (topo_edge_t))) == NULL ||
	    (e_to = topo_mod_zalloc(mod, sizeof (topo_edge_t))) == NULL) {
		topo_mod_free(mod, e_from, sizeof (topo_edge_t));
		topo_mod_free(mod, e_to, sizeof (topo_edge_t));
		return (topo_mod_seterrno(mod, EMOD_NOMEM));
	}
	e_from->tve_vertex = from;
	e_to->tve_vertex = to;

	(void) pthread_mutex_lock(&tdg->tdg_lock);
	topo_list_append(&from->tvt_outgoing, e_to);
	from->tvt_noutgoing++;
	topo_list_append(&to->tvt_incoming, e_from);
	to->tvt_nincoming++;
	tdg->tdg_nedges++;
	(void) pthread_mutex_unlock(&tdg->tdg_lock);

	return (0);
}

int
topo_edge_iter(topo_hdl_t *thp, topo_vertex_t *vtx,
    int (*func)(topo_hdl_t *, topo_edge_t *, boolean_t, void *), void *arg)
{
	uint_t n = 0;

	for (topo_edge_t *edge = topo_list_next(&vtx->tvt_outgoing);
	    edge != NULL; edge = topo_list_next(edge), n++) {
		int ret;
		boolean_t last_edge = B_FALSE;

		if (n == (vtx->tvt_noutgoing - 1))
			last_edge = B_TRUE;

		ret = func(thp, edge, last_edge, arg);

		switch (ret) {
		case (TOPO_WALK_NEXT):
			continue;
		case (TOPO_WALK_TERMINATE):
			break;
		case (TOPO_WALK_ERR):
			/* FALLTHRU */
		default:
			return (-1);
		}
	}
	return (0);
}

/*
 * Convenience interface for deallocating a topo_path_t
 */
void
topo_path_destroy(topo_hdl_t *thp, topo_path_t *path)
{
	topo_path_component_t *pathcomp;

	if (path == NULL)
		return;

	topo_hdl_strfree(thp, (char *)path->tsp_fmristr);
	nvlist_free(path->tsp_fmri);

	pathcomp = topo_list_next(&path->tsp_components);
	while (pathcomp != NULL) {
		topo_path_component_t *tmp = pathcomp;

		pathcomp = topo_list_next(pathcomp);
		topo_hdl_free(thp, tmp, sizeof (topo_path_component_t));
	}

	topo_hdl_free(thp, path, sizeof (topo_path_t));
}

/*
 * This just wraps topo_path_t so that visit_vertex() can build a linked list
 * of paths.
 */
struct digraph_path
{
	topo_list_t	dgp_link;
	topo_path_t	*dgp_path;
};

static int
visit_vertex(topo_hdl_t *thp, topo_vertex_t *vtx, topo_vertex_t *to,
    topo_list_t *all_paths, char *curr_path, topo_list_t *curr_path_comps,
    uint_t *npaths)
{
	struct digraph_path *pathnode = NULL;
	topo_path_t *path = NULL;
	topo_path_component_t *pathcomp = NULL;
	nvlist_t *fmri = NULL;
	char *pathstr;
	int err;

	(void) asprintf(&pathstr, "%s/%s=%" PRIx64"",
	    curr_path,
	    topo_node_name(vtx->tvt_node),
	    topo_node_instance(vtx->tvt_node));

	if ((pathcomp = topo_hdl_zalloc(thp, sizeof (topo_path_component_t)))
	    == NULL) {
		(void) topo_hdl_seterrno(thp, ETOPO_NOMEM);
		goto err;
	}
	pathcomp->tspc_vertex = vtx;
	topo_list_append(curr_path_comps, pathcomp);

	if (vtx == to) {
		(*npaths)++;
		pathnode = topo_hdl_zalloc(thp, sizeof (struct digraph_path));

		if ((path = topo_hdl_zalloc(thp, sizeof (topo_path_t))) ==
		    NULL ||
		    (path->tsp_fmristr = topo_hdl_strdup(thp, pathstr)) ==
		    NULL) {
			(void) topo_hdl_seterrno(thp, ETOPO_NOMEM);
			goto err;
		}

		if (topo_list_deepcopy(thp, &path->tsp_components,
		    curr_path_comps, sizeof (topo_path_component_t)) != 0) {
			(void) topo_hdl_seterrno(thp, ETOPO_NOMEM);
		}
		if (topo_fmri_str2nvl(thp, pathstr, &fmri, &err) != 0) {
			/* errno set */
			goto err;
		}
		path->tsp_fmri = fmri;
		pathnode->dgp_path = path;

		topo_list_append(all_paths, pathnode);
		free(pathstr);
		return (0);
	}

	for (topo_edge_t *edge = topo_list_next(&vtx->tvt_outgoing);
	    edge != NULL; edge = topo_list_next(edge)) {

		if (visit_vertex(thp, edge->tve_vertex, to, all_paths, pathstr,
		    curr_path_comps, npaths) != 0)
			goto err;
	}

	free(pathstr);
	topo_list_delete(curr_path_comps, pathcomp);
	topo_hdl_free(thp, pathcomp, sizeof (topo_path_component_t));
	return (0);

err:
	topo_hdl_free(thp, pathnode, sizeof (struct digraph_path));
	topo_path_destroy(thp, path);
	return (-1);
}

/*
 * On success, populates the "paths" parameter with an array of
 * topo_saspath_t structs representing all paths from the "from" vertex to the
 * "to" vertex.  The caller is responsible for freeing this array.  Also, on
 * success, returns the the number of paths found.  If no paths are found, 0
 * is returned.
 *
 * On error, -1 is returned.
 */
int
topo_digraph_paths(topo_hdl_t *thp, topo_digraph_t *tdg, topo_vertex_t *from,
    topo_vertex_t *to, topo_path_t ***paths)
{
	topo_list_t all_paths = { 0 };
	char *curr_path;
	topo_path_component_t *pathcomp = NULL;
	topo_list_t curr_path_comps = { 0 };
	struct digraph_path *path;
	uint_t i, npaths = 0;
	int ret;

	ret = asprintf(&curr_path, "sas://%s=%s/%s=%" PRIx64"",
	    FM_FMRI_SAS_TYPE, FM_FMRI_SAS_TYPE_PATH,
	    topo_node_name(from->tvt_node),
	    topo_node_instance(from->tvt_node));

	if ((pathcomp = topo_hdl_zalloc(thp, sizeof (topo_path_component_t)))
	    == NULL) {
		(void) topo_hdl_seterrno(thp, ETOPO_NOMEM);
		goto err;
	}
	pathcomp->tspc_vertex = from;
	topo_list_append(&curr_path_comps, pathcomp);

	if (ret == -1)
		return (topo_hdl_seterrno(thp, ETOPO_NOMEM));

	for (topo_edge_t *edge = topo_list_next(&from->tvt_outgoing);
	    edge != NULL; edge = topo_list_next(edge)) {

		ret = visit_vertex(thp, edge->tve_vertex, to, &all_paths,
		    curr_path, &curr_path_comps, &npaths);
		if (ret != 0) {
			/* errno set */
			free(curr_path);
			goto err;
		}
	}
	free(curr_path);
	topo_hdl_free(thp, pathcomp, sizeof (topo_path_component_t));

	/*
	 * No paths were found between the "from" and "to" vertices, so
	 * we're done here.
	 */
	if (npaths == 0)
		return (0);

	*paths = topo_hdl_zalloc(thp, npaths * sizeof (topo_path_t *));
	if (*paths == NULL) {
		(void) topo_hdl_seterrno(thp, ETOPO_NOMEM);
		goto err;
	}

	for (i = 0, path = topo_list_next(&all_paths); path != NULL;
	    i++, path = topo_list_next(path)) {

		*((*paths) + i) = path->dgp_path;
	}

	path = topo_list_next(&all_paths);
	while (path != NULL) {
		struct digraph_path *tmp = path;

		path = topo_list_next(path);
		topo_hdl_free(thp, tmp, sizeof (struct digraph_path));
	}
	return (npaths);

err:
	path = topo_list_next(&all_paths);
	while (path != NULL) {
		struct digraph_path *tmp = path;

		path = topo_list_next(path);
		topo_hdl_free(thp, tmp, sizeof (struct digraph_path));
	}

	topo_dprintf(thp, TOPO_DBG_ERR, "%s: failed (%s)", __func__,
	    topo_hdl_errmsg(thp));
	return (-1);
}
