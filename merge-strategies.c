#include "cache.h"
#include "cache-tree.h"
#include "dir.h"
#include "lockfile.h"
#include "merge-strategies.h"
#include "unpack-trees.h"
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

static int fast_forward(struct repository *r, struct tree_desc *t,
			int nr, int aggressive)
{
	struct unpack_trees_options opts;
	struct lock_file lock = LOCK_INIT;

	refresh_index(r->index, REFRESH_QUIET, NULL, NULL, NULL);
	repo_hold_locked_index(r, &lock, LOCK_DIE_ON_ERROR);

	memset(&opts, 0, sizeof(opts));
	opts.head_idx = 1;
	opts.src_index = r->index;
	opts.dst_index = r->index;
	opts.merge = 1;
	opts.update = 1;
	opts.aggressive = aggressive;

	if (nr == 1)
		opts.fn = oneway_merge;
	else if (nr == 2) {
		opts.fn = twoway_merge;
		opts.initial_checkout = is_index_unborn(r->index);
	} else if (nr >= 3) {
		opts.fn = threeway_merge;
		opts.head_idx = nr - 1;
	}

	if (unpack_trees(nr, t, &opts))
		return -1;

	if (write_locked_index(r->index, &lock, COMMIT_LOCK))
		return error(_("unable to write new index file"));

	return 0;
}

static int add_tree(struct tree *tree, struct tree_desc *t)
{
	if (parse_tree(tree))
		return -1;

	init_tree_desc(t, tree->buffer, tree->size);
	return 0;
}

int merge_strategies_resolve(struct repository *r,
			     struct commit_list *bases, const char *head_arg,
			     struct commit_list *remote)
{
	struct tree_desc t[MAX_UNPACK_TREES];
	struct object_id head, oid;
	struct commit_list *i;
	int nr = 0;

	if (head_arg)
		get_oid(head_arg, &head);

	puts(_("Trying simple merge."));

	for (i = bases; i && i->item; i = i->next) {
		if (add_tree(repo_get_commit_tree(r, i->item), t + (nr++)))
			return 2;
	}

	if (head_arg) {
		struct tree *tree = parse_tree_indirect(&head);
		if (add_tree(tree, t + (nr++)))
			return 2;
	}

	if (remote && add_tree(repo_get_commit_tree(r, remote->item), t + (nr++)))
		return 2;

	if (fast_forward(r, t, nr, 1))
		return 2;

	if (write_index_as_tree(&oid, r->index, r->index_file,
				WRITE_TREE_SILENT, NULL)) {
		int ret;
		struct lock_file lock = LOCK_INIT;

		puts(_("Simple merge failed, trying Automatic merge."));
		repo_hold_locked_index(r, &lock, LOCK_DIE_ON_ERROR);
		ret = merge_all_index(r->index, 1, 0, merge_one_file_func, NULL);

		write_locked_index(r->index, &lock, COMMIT_LOCK);
		return !!ret;
	}

	return 0;
}
