#include "cache.h"
#include "cache-tree.h"
#include "dir.h"
#include "lockfile.h"
#include "merge-strategies.h"
#include "run-command.h"
#include "unpack-trees.h"
#include "xdiff-interface.h"

static int checkout_from_index(struct index_state *istate, const char *path,
			       struct cache_entry *ce)
{
	struct checkout state = CHECKOUT_INIT;

	state.istate = istate;
	state.force = 1;
	state.base_dir = "";
	state.base_dir_len = 0;

	if (checkout_entry(ce, &state, NULL, NULL) < 0)
		return error(_("%s: cannot checkout file"), path);
	return 0;
}

static int merge_one_file_deleted(struct index_state *istate,
				  const struct object_id *our_blob,
				  const struct object_id *their_blob, const char *path,
				  unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode)
{
	if ((our_blob && orig_mode != our_mode) ||
	    (their_blob && orig_mode != their_mode))
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
	else if (our_mode != their_mode)
		return error(_("permission conflict: %o->%o,%o in %s"),
			     orig_mode, our_mode, their_mode, path);

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
	} else if (ret > 0 || !orig_blob) {
		free(result.ptr);
		return error(_("content conflict in %s"), path);
	}

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

	return add_file_to_index(istate, path, 0);
}

int merge_three_way(struct repository *r,
		    const struct object_id *orig_blob,
		    const struct object_id *our_blob,
		    const struct object_id *their_blob, const char *path,
		    unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode)
{
	if (orig_blob &&
	    ((!their_blob && our_blob && oideq(orig_blob, our_blob)) ||
	     (!our_blob && their_blob && oideq(orig_blob, their_blob)))) {
		/* Deleted in both or deleted in one and unchanged in the other. */
		return merge_one_file_deleted(r->index, our_blob, their_blob, path,
					      orig_mode, our_mode, their_mode);
	} else if (!orig_blob && our_blob && !their_blob) {
		/*
		 * Added in one.  The other side did not add and we
		 * added so there is nothing to be done, except making
		 * the path merged.
		 */
		return add_to_index_cacheinfo(r->index, our_mode, our_blob, path, 0, 1, 1, NULL);
	} else if (!orig_blob && !our_blob && their_blob) {
		struct cache_entry *ce;
		printf(_("Adding %s\n"), path);

		if (file_exists(path))
			return error(_("untracked %s is overwritten by the merge."), path);

		if (add_to_index_cacheinfo(r->index, their_mode, their_blob, path, 0, 1, 1, &ce))
			return -1;
		return checkout_from_index(r->index, path, ce);
	} else if (!orig_blob && our_blob && their_blob &&
		   oideq(our_blob, their_blob)) {
		struct cache_entry *ce;

		/* Added in both, identically (check for same permissions). */
		if (our_mode != their_mode)
			return error(_("File %s added identically in both branches, "
				       "but permissions conflict %o->%o."),
				     path, our_mode, their_mode);

		printf(_("Adding %s\n"), path);

		if (add_to_index_cacheinfo(r->index, our_mode, our_blob, path, 0, 1, 1, &ce))
			return -1;
		return checkout_from_index(r->index, path, ce);
	} else if (our_blob && their_blob) {
		/* Modified in both, but differently. */
		return do_merge_one_file(r->index,
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

int merge_one_file_func(const struct object_id *orig_blob,
			const struct object_id *our_blob,
			const struct object_id *their_blob, const char *path,
			unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode,
			void *data)
{
	return merge_three_way((struct repository *)data,
			       orig_blob, our_blob, their_blob, path,
			       orig_mode, our_mode, their_mode);
}

int merge_one_file_spawn(const struct object_id *orig_blob,
			 const struct object_id *our_blob,
			 const struct object_id *their_blob, const char *path,
			 unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode,
			 void *data)
{
	char oids[3][GIT_MAX_HEXSZ + 1] = {{0}};
	char modes[3][10] = {{0}};
	const char *arguments[] = { (char *)data, oids[0], oids[1], oids[2],
				    path, modes[0], modes[1], modes[2], NULL };

	if (orig_blob) {
		oid_to_hex_r(oids[0], orig_blob);
		xsnprintf(modes[0], sizeof(modes[0]), "%06o", orig_mode);
	}

	if (our_blob) {
		oid_to_hex_r(oids[1], our_blob);
		xsnprintf(modes[1], sizeof(modes[1]), "%06o", our_mode);
	}

	if (their_blob) {
		oid_to_hex_r(oids[2], their_blob);
		xsnprintf(modes[2], sizeof(modes[2]), "%06o", their_mode);
	}

	return run_command_v_opt(arguments, 0);
}

static int merge_entry(struct index_state *istate, int quiet, int pos,
		       const char *path, merge_fn fn, void *data)
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

	if (fn(oids[0], oids[1], oids[2], path, modes[0], modes[1], modes[2], data)) {
		if (!quiet)
			error(_("Merge program failed"));
		return -2;
	}

	return found;
}

int merge_index_path(struct index_state *istate, int oneshot, int quiet,
		     const char *path, merge_fn fn, void *data)
{
	int pos = index_name_pos(istate, path, strlen(path)), ret;

	/*
	 * If it already exists in the cache as stage0, it's
	 * already merged and there is nothing to do.
	 */
	if (pos < 0) {
		ret = merge_entry(istate, quiet, -pos - 1, path, fn, data);
		if (ret == -1)
			return -1;
		else if (ret == -2)
			return 1;
	}
	return 0;
}

int merge_all_index(struct index_state *istate, int oneshot, int quiet,
		    merge_fn fn, void *data)
{
	int err = 0, i, ret;
	for (i = 0; i < istate->cache_nr; i++) {
		const struct cache_entry *ce = istate->cache[i];
		if (!ce_stage(ce))
			continue;

		ret = merge_entry(istate, quiet, i, ce->name, fn, data);
		if (ret > 0)
			i += ret - 1;
		else if (ret == -1)
			return -1;
		else if (ret == -2) {
			if (oneshot)
				err++;
			else
				return 1;
		}
	}

	return err;
}

static int add_tree(const struct object_id *oid, struct tree_desc *t)
{
	struct tree *tree;

	tree = parse_tree_indirect(oid);
	if (parse_tree(tree))
		return -1;

	init_tree_desc(t, tree->buffer, tree->size);
	return 0;
}

int merge_strategies_resolve(struct repository *r,
			     struct commit_list *bases, const char *head_arg,
			     struct commit_list *remote)
{
	int i = 0;
	struct lock_file lock = LOCK_INIT;
	struct tree_desc t[MAX_UNPACK_TREES];
	struct unpack_trees_options opts;
	struct object_id head, oid;
	struct commit_list *j;

	if (head_arg)
		get_oid(head_arg, &head);

	repo_hold_locked_index(r, &lock, LOCK_DIE_ON_ERROR);
	refresh_index(r->index, 0, NULL, NULL, NULL);

	memset(&opts, 0, sizeof(opts));
	opts.head_idx = 1;
	opts.src_index = r->index;
	opts.dst_index = r->index;
	opts.update = 1;
	opts.merge = 1;
	opts.aggressive = 1;

	for (j = bases; j && j->item; j = j->next) {
		if (add_tree(&j->item->object.oid, t + (i++)))
			goto out;
	}

	if (head_arg && add_tree(&head, t + (i++)))
		goto out;
	if (remote && add_tree(&remote->item->object.oid, t + (i++)))
		goto out;

	if (i == 1)
		opts.fn = oneway_merge;
	else if (i == 2) {
		opts.fn = twoway_merge;
		opts.initial_checkout = is_index_unborn(r->index);
	} else if (i >= 3) {
		opts.fn = threeway_merge;
		opts.head_idx = i - 1;
	}

	if (unpack_trees(i, t, &opts))
		goto out;

	puts(_("Trying simple merge."));
	write_locked_index(r->index, &lock, COMMIT_LOCK);

	if (write_index_as_tree(&oid, r->index, r->index_file,
				WRITE_TREE_SILENT, NULL)) {
		int ret;

		puts(_("Simple merge failed, trying Automatic merge."));
		repo_hold_locked_index(r, &lock, LOCK_DIE_ON_ERROR);
		ret = merge_all_index(r->index, 0, 0, merge_one_file_func, r);

		write_locked_index(r->index, &lock, COMMIT_LOCK);
		return !!ret;
	}

	return 0;

 out:
	rollback_lock_file(&lock);
	return 2;
}
