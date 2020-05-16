#include "cache.h"
#include "dir.h"
#include "merge-strategies.h"
#include "xdiff-interface.h"

static int add_to_index_cacheinfo(struct index_state *istate,
				  unsigned int mode,
				  const struct object_id *oid, const char *path)
{
	struct cache_entry *ce;
	int len, option;

	if (!verify_path(path, mode))
		return error("Invalid path '%s'", path);

	len = strlen(path);
	ce = make_empty_cache_entry(istate, len);

	oidcpy(&ce->oid, oid);
	memcpy(ce->name, path, len);
	ce->ce_flags = create_ce_flags(0);
	ce->ce_namelen = len;
	ce->ce_mode = create_ce_mode(mode);
	if (assume_unchanged)
		ce->ce_flags |= CE_VALID;
	option = ADD_CACHE_OK_TO_ADD | ADD_CACHE_OK_TO_REPLACE;
	if (add_index_entry(istate, ce, option))
		return error("%s: cannot add to the index", path);

	return 0;
}

static int checkout_from_index(struct index_state *istate, const char *path)
{
	struct checkout state;
	struct cache_entry *ce;

	state.istate = istate;
	state.force = 1;
	state.base_dir = "";
	state.base_dir_len = 0;

	ce = index_file_exists(istate, path, strlen(path), 0);
	if (checkout_entry(ce, &state, NULL, NULL) < 0)
		return error("%s: cannot checkout file", path);
	return 0;
}

static int merge_one_file_deleted(struct index_state *istate,
				  const struct object_id *orig_blob,
				  const struct object_id *our_blob,
				  const struct object_id *their_blob, const char *path,
				  unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode)
{
	if ((our_blob && orig_mode != our_mode) ||
	    (their_blob && orig_mode != their_mode))
		return error(_("File %s deleted on one branch but had its "
			       "permissions changed on the other."), path);

	if (our_blob) {
		printf("Removing %s\n", path);

		if (file_exists(path))
			remove_path(path);
	}

	if (remove_file_from_index(istate, path))
		return error("%s: cannot remove from the index", path);
	return 0;
}

static int do_merge_one_file(struct index_state *istate,
			     const struct object_id *orig_blob,
			     const struct object_id *our_blob,
			     const struct object_id *their_blob, const char *path,
			     unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode)
{
	int ret, i, dest;
	mmbuffer_t result = {NULL, 0};
	mmfile_t mmfs[3];
	xmparam_t xmp = {{0}};
	struct cache_entry *ce;

	if (our_mode == S_IFLNK || their_mode == S_IFLNK)
		return error(_("%s: Not merging symbolic link changes."), path);
	else if (our_mode == S_IFGITLINK || their_mode == S_IFGITLINK)
		return error(_("%s: Not merging conflicting submodule changes."), path);

	read_mmblob(mmfs + 0, our_blob);
	read_mmblob(mmfs + 2, their_blob);

	if (orig_blob) {
		printf("Auto-merging %s\n", path);
		read_mmblob(mmfs + 1, orig_blob);
	} else {
		printf("Added %s in both, but differently.\n", path);
		read_mmblob(mmfs + 1, the_hash_algo->empty_blob);
	}

	xmp.level = XDL_MERGE_ZEALOUS_ALNUM;
	xmp.style = 0;
	xmp.favor = 0;

	ret = xdl_merge(mmfs + 1, mmfs + 0, mmfs + 2, &xmp, &result);

	for (i = 0; i < 3; i++)
		free(mmfs[i].ptr);

	if (ret > 127)
		ret = 1;

	ce = index_file_exists(istate, path, strlen(path), 0);
	if (!ce)
		BUG("file is not present in the cache?");

	unlink(path);
	dest = open(path, O_WRONLY | O_CREAT, ce->ce_mode);
	write_in_full(dest, result.ptr, result.size);
	close(dest);

	free(result.ptr);

	if (ret) {
		if (!orig_blob)
			error(_("content conflict in %s"), path);
		if (our_mode != their_mode)
			error(_("permission conflict: %o->%o,%o in %s"),
			      orig_mode, our_mode, their_mode, path);

		return 1;
	}

	return add_file_to_index(istate, path, 0);
}

int merge_strategies_one_file(struct repository *r,
			      const struct object_id *orig_blob,
			      const struct object_id *our_blob,
			      const struct object_id *their_blob, const char *path,
			      unsigned int orig_mode, unsigned int our_mode,
			      unsigned int their_mode)
{
	if (orig_blob &&
	    ((our_blob && oideq(orig_blob, our_blob)) ||
	     (their_blob && oideq(orig_blob, their_blob))))
		return merge_one_file_deleted(r->index,
					      orig_blob, our_blob, their_blob, path,
					      orig_mode, our_mode, their_mode);
	else if (!orig_blob && our_blob && !their_blob) {
		return add_to_index_cacheinfo(r->index, our_mode, our_blob, path);
	} else if (!orig_blob && !our_blob && their_blob) {
		printf("Adding %s\n", path);

		if (file_exists(path))
			return error(_("untracked %s is overwritten by the merge.\n"), path);

		if (add_to_index_cacheinfo(r->index, their_mode, their_blob, path))
			return 1;
		return checkout_from_index(r->index, path);
	} else if (!orig_blob && our_blob && their_blob &&
		   oideq(our_blob, their_blob)) {
		if (our_mode != their_mode)
			return error(_("File %s added identically in both branches, "
				       "but permissions conflict %o->%o."),
				     path, our_mode, their_mode);

		printf("Adding %s\n", path);

		if (add_to_index_cacheinfo(r->index, our_mode, our_blob, path))
			return 1;
		return checkout_from_index(r->index, path);
	} else if (our_blob && their_blob)
		return do_merge_one_file(r->index,
					 orig_blob, our_blob, their_blob, path,
					 orig_mode, our_mode, their_mode);
	else {
		char *orig_hex = "", *our_hex = "", *their_hex = "";

		if (orig_blob)
			orig_hex = oid_to_hex(orig_blob);
		if (our_blob)
			our_hex = oid_to_hex(our_blob);
		if (their_blob)
			their_hex = oid_to_hex(their_blob);

		return error(_("%s: Not handling case %s -> %s -> %s"),
			path, orig_hex, our_hex, their_hex);
	}

	return 0;
}
