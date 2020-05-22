/*
 * Builtin "git merge-resolve"
 *
 * Copyright (c) 2020 Alban Gruin
 *
 * Based on git-merge-resolve.sh, written by Linus Torvalds and Junio C
 * Hamano.
 *
 * Resolve two trees, using enhanced multi-base read-tree.
 */

#include "cache.h"
#include "cache-tree.h"
#include "builtin.h"
#include "lockfile.h"
#include "merge-strategies.h"
#include "unpack-trees.h"

static int add_tree(const struct object_id *oid, struct tree_desc *t)
{
	struct tree *tree;

	tree = parse_tree_indirect(oid);
	if (parse_tree(tree))
		return -1;

	init_tree_desc(t, tree->buffer, tree->size);
	return 0;
}

static int merge_resolve(struct commit_list *bases, const char *head_arg,
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

	repo_hold_locked_index(the_repository, &lock, LOCK_DIE_ON_ERROR);
	refresh_index(the_repository->index, 0, NULL, NULL, NULL);

	memset(&opts, 0, sizeof(opts));
	opts.head_idx = 1;
	opts.src_index = the_repository->index;
	opts.dst_index = the_repository->index;
	opts.update = 1;
	opts.merge = 1;
	opts.aggressive = 1;

	for (j = bases; j; j = j->next) {
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
		opts.initial_checkout = is_index_unborn(the_repository->index);
	} else if (i >= 3) {
		opts.fn = threeway_merge;
		opts.head_idx = i - 1;
	}

	if (unpack_trees(i, t, &opts))
		goto out;

	puts("Trying simple merge.");
	write_locked_index(the_repository->index, &lock, COMMIT_LOCK);

	if (write_index_as_tree(&oid, the_repository->index,
				the_repository->index_file, 0, NULL)) {
		int ret;

		repo_hold_locked_index(the_repository, &lock, LOCK_DIE_ON_ERROR);
		ret = merge_all(the_repository->index, 0, 0,
				merge_one_file_cb, the_repository);

		write_locked_index(the_repository->index, &lock, COMMIT_LOCK);
		return !!ret;
	}

	return 0;

 out:
	rollback_lock_file(&lock);
	return 2;
}

static const char builtin_merge_resolve_usage[] =
	"git merge-resolve <bases>... -- <head> <remote>";

int cmd_merge_resolve(int argc, const char **argv, const char *prefix)
{
	int i, is_baseless = 1, sep_seen = 0;
	const char *head = NULL;
	struct commit_list *bases = NULL, *remote = NULL;
	struct commit_list **next_base = &bases;

	if (argc < 5)
		usage(builtin_merge_resolve_usage);

	setup_work_tree();
	if (repo_read_index(the_repository) < 0)
		die("invalid index");

	/* The first parameters up to -- are merge bases; the rest are
	 * heads. */
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--") == 0)
			sep_seen = 1;
		else if (strcmp(argv[i], "-h") == 0)
			usage(builtin_merge_resolve_usage);
		else if (sep_seen && !head)
			head = argv[i];
		else if (remote) {
			/* Give up if we are given two or more remotes.
			 * Not handling octopus. */
			return 2;
		} else {
			struct object_id oid;

			get_oid(argv[i], &oid);
			is_baseless &= sep_seen;

			if (!oideq(&oid, the_hash_algo->empty_tree)) {
				struct commit *commit;
				commit = lookup_commit_or_die(&oid, argv[i]);

				if (sep_seen)
					commit_list_append(commit, &remote);
				else
					next_base = commit_list_append(commit, next_base);
			}
		}
	}

	/* Give up if this is a baseless merge. */
	if (is_baseless)
		return 2;

	return merge_resolve(bases, head, remote);
}
