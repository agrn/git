#include "cache.h"
#include "dir.h"
#include "merge-strategies.h"
#include "xdiff-interface.h"

static int add_merge_result_to_index(struct index_state *istate, unsigned int mode,
				     const struct object_id *oid, const char *path,
				     int checkout)
{
	struct cache_entry *ce;
	int res;

	res = add_to_index_cacheinfo(istate, mode, oid, path, 0, 1, 1, &ce);
	if (res == -1)
		return error(_("Invalid path '%s'"), path);
	else if (res == -2)
		return -1;

	if (checkout) {
		struct checkout state = CHECKOUT_INIT;

		state.istate = istate;
		state.force = 1;
		state.base_dir = "";
		state.base_dir_len = 0;

		if (checkout_entry(ce, &state, NULL, NULL) < 0)
			return error(_("%s: cannot checkout file"), path);
	}

	return 0;
}

static int merge_one_file_deleted(struct index_state *istate,
				  const struct object_id *our_blob,
				  const struct object_id *their_blob, const char *path,
				  unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode)
{
	if ((!our_blob && orig_mode != their_mode) ||
	    (!their_blob && orig_mode != our_mode))
		return error(_("File %s deleted on one branch but had its "
			       "permissions changed on the other."), path);

	if (our_blob) {
		printf(_("Removing %s\n"), path);

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
	ssize_t written;
	mmbuffer_t result = {NULL, 0};
	mmfile_t mmfs[3];
	xmparam_t xmp = {{0}};

	if (our_mode == S_IFLNK || their_mode == S_IFLNK)
		return error(_("%s: Not merging symbolic link changes."), path);
	else if (our_mode == S_IFGITLINK || their_mode == S_IFGITLINK)
		return error(_("%s: Not merging conflicting submodule changes."), path);

	if (orig_blob) {
		printf(_("Auto-merging %s\n"), path);
		read_mmblob(mmfs + 0, orig_blob);
	} else {
		printf(_("Added %s in both, but differently.\n"), path);
		read_mmblob(mmfs + 0, &null_oid);
	}

	read_mmblob(mmfs + 1, our_blob);
	read_mmblob(mmfs + 2, their_blob);

	xmp.level = XDL_MERGE_ZEALOUS_ALNUM;
	xmp.style = 0;
	xmp.favor = 0;

	ret = xdl_merge(mmfs + 0, mmfs + 1, mmfs + 2, &xmp, &result);

	for (i = 0; i < 3; i++)
		free(mmfs[i].ptr);

	if (ret < 0) {
		free(result.ptr);
		return error(_("Failed to execute internal merge"));
	}

	if (ret > 0 || !orig_blob)
		ret = error(_("content conflict in %s"), path);
	if (our_mode != their_mode)
		ret = error(_("permission conflict: %o->%o,%o in %s"),
			    orig_mode, our_mode, their_mode, path);

	unlink(path);
	if ((dest = open(path, O_WRONLY | O_CREAT, our_mode)) < 0) {
		free(result.ptr);
		return error_errno(_("failed to open file '%s'"), path);
	}

	written = write_in_full(dest, result.ptr, result.size);
	close(dest);

	free(result.ptr);

	if (written < 0)
		return error_errno(_("failed to write to '%s'"), path);
	if (ret)
		return ret;

	return add_file_to_index(istate, path, 0);
}

int merge_three_way(struct index_state *istate,
		    const struct object_id *orig_blob,
		    const struct object_id *our_blob,
		    const struct object_id *their_blob, const char *path,
		    unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode)
{
	if (orig_blob &&
	    ((!our_blob && !their_blob) ||
	     (!their_blob && our_blob && oideq(orig_blob, our_blob)) ||
	     (!our_blob && their_blob && oideq(orig_blob, their_blob)))) {
		/* Deleted in both or deleted in one and unchanged in the other. */
		return merge_one_file_deleted(istate, our_blob, their_blob, path,
					      orig_mode, our_mode, their_mode);
	} else if (!orig_blob && our_blob && !their_blob) {
		/*
		 * Added in ours.  The other side did not add and we
		 * added so there is nothing to be done, except making
		 * the path merged.
		 */
		return add_merge_result_to_index(istate, our_mode, our_blob, path, 0);
	} else if (!orig_blob && !our_blob && their_blob) {
		printf(_("Adding %s\n"), path);

		if (file_exists(path))
			return error(_("untracked %s is overwritten by the merge."), path);

		return add_merge_result_to_index(istate, their_mode, their_blob, path, 1);
	} else if (!orig_blob && our_blob && their_blob &&
		   oideq(our_blob, their_blob)) {
		/* Added in both, identically (check for same permissions). */
		if (our_mode != their_mode)
			return error(_("File %s added identically in both branches, "
				       "but permissions conflict %o->%o."),
				     path, our_mode, their_mode);

		printf(_("Adding %s\n"), path);

		return add_merge_result_to_index(istate, our_mode, our_blob, path, 1);
	} else if (our_blob && their_blob) {
		/* Modified in both, but differently. */
		return do_merge_one_file(istate,
					 orig_blob, our_blob, their_blob, path,
					 orig_mode, our_mode, their_mode);
	} else {
		char orig_hex[GIT_MAX_HEXSZ] = {0}, our_hex[GIT_MAX_HEXSZ] = {0},
			their_hex[GIT_MAX_HEXSZ] = {0};

		if (orig_blob)
			oid_to_hex_r(orig_hex, orig_blob);
		if (our_blob)
			oid_to_hex_r(our_hex, our_blob);
		if (their_blob)
			oid_to_hex_r(their_hex, their_blob);

		return error(_("%s: Not handling case %s -> %s -> %s"),
			     path, orig_hex, our_hex, their_hex);
	}

	return 0;
}

int merge_one_file_func(struct index_state *istate,
			const struct object_id *orig_blob,
			const struct object_id *our_blob,
			const struct object_id *their_blob, const char *path,
			unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode,
			void *data)
{
	return merge_three_way(istate,
			       orig_blob, our_blob, their_blob, path,
			       orig_mode, our_mode, their_mode);
}

static int merge_entry(struct index_state *istate, int quiet, unsigned int pos,
		       const char *path, int *err, merge_fn fn, void *data)
{
	int found = 0;
	const struct object_id *oids[3] = {NULL};
	unsigned int modes[3] = {0};

	do {
		const struct cache_entry *ce = istate->cache[pos];
		int stage = ce_stage(ce);

		if (strcmp(ce->name, path))
			break;
		found++;
		oids[stage - 1] = &ce->oid;
		modes[stage - 1] = ce->ce_mode;
	} while (++pos < istate->cache_nr);
	if (!found)
		return error(_("%s is not in the cache"), path);

	if (fn(istate, oids[0], oids[1], oids[2], path,
	       modes[0], modes[1], modes[2], data)) {
		if (!quiet)
			error(_("Merge program failed"));
		(*err)++;
	}

	return found;
}

int merge_index_path(struct index_state *istate, int oneshot, int quiet,
		     const char *path, merge_fn fn, void *data)
{
	int pos = index_name_pos(istate, path, strlen(path)), ret, err = 0;

	/*
	 * If it already exists in the cache as stage0, it's
	 * already merged and there is nothing to do.
	 */
	if (pos < 0) {
		ret = merge_entry(istate, quiet || oneshot, -pos - 1, path, &err, fn, data);
		if (ret == -1)
			return -1;
		else if (err)
			return 1;
	}
	return 0;
}

int merge_all_index(struct index_state *istate, int oneshot, int quiet,
		    merge_fn fn, void *data)
{
	int err = 0, ret;
	unsigned int i, prev_nr;

	for (i = 0; i < istate->cache_nr; i++) {
		const struct cache_entry *ce = istate->cache[i];
		if (!ce_stage(ce))
			continue;

		prev_nr = istate->cache_nr;
		ret = merge_entry(istate, quiet || oneshot, i, ce->name, &err, fn, data);
		if (ret > 0) {
			/*
			 * Don't bother handling an index that has
			 * grown, since merge_one_file_func() can't grow
			 * it, and merge_one_file_spawn() can't change
			 * it.
			 */
			i += ret - (prev_nr - istate->cache_nr) - 1;
		} else if (ret == -1)
			return -1;

		if (err && !oneshot)
			return 1;
	}

	return err;
}
