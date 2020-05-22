/* Resolve two trees, using enhanced multi-base read-tree.  Based on
 * git-merge-resolve.sh, which is Copyright (c) 2005 Linus Torvalds and
 * Junio C Hamano.  The rewritten version is Copyright (c) 2020 Alban
 * Gruin. */

#include "cache.h"
#include "cache-tree.h"
#include "builtin.h"
#include "lockfile.h"
#include "run-command.h"
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

static int merge_resolve(struct oid_array *bases, const struct object_id *head,
			 const struct object_id *remote)
{
	int i;
	struct lock_file lock = LOCK_INIT;
	struct tree_desc t[MAX_UNPACK_TREES];
	struct unpack_trees_options opts;
	struct object_id oid;

	repo_hold_locked_index(the_repository, &lock, LOCK_DIE_ON_ERROR);
	refresh_index(the_repository->index, 0, NULL, NULL, NULL);

	memset(&opts, 0, sizeof(opts));
	opts.src_index = the_repository->index;
	opts.dst_index = the_repository->index;
	opts.update = 1;
	opts.merge = 1;
	opts.aggressive = 1;

	for (i = 0; i < bases->nr; i++) {
		if (add_tree(bases->oid + i, t + i))
			goto out;
	}

	if (head && add_tree(head, t + (++i)))
		goto out;
	if (remote && add_tree(remote, t + (++i)))
		goto out;

	opts.head_idx = 1;
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
		struct child_process cp_merge = CHILD_PROCESS_INIT;

		puts("Simple merge failed, trying Automatic merge.");

		cp_merge.git_cmd = 1;
		argv_array_pushl(&cp_merge.args, "merge-index", "-o",
				 "git-merge-one-file", "-a", NULL);
		if (run_command(&cp_merge))
			return 1;
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
	int i, sep_seen = 0;
	struct oid_array bases = OID_ARRAY_INIT;
	struct object_id head, remote,
		*p_head = NULL, *p_remote = NULL;

	if (argc < 5)
		usage(builtin_merge_resolve_usage);

	setup_work_tree();
	if (repo_read_index(the_repository) < 0)
		die("invalid index");

	/* The first parameters up to -- are merge bases; the rest are
	 * heads. */
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "==") == 0)
			sep_seen = 1;
		else if (strcmp(argv[i], "-h") == 0)
			usage(builtin_merge_resolve_usage);
		else if (sep_seen && !p_head) {
			get_oid(argv[i], &head);
			p_head = &head;
		} else if (sep_seen && p_head && !p_remote) {
			get_oid(argv[i], &remote);
			p_remote = &remote;
		} else if (p_remote) {
			/* Give up if we are given two or more remotes.
			 * Not handling octopus. */
			return 2;
		} else {
			struct object_id oid;

			get_oid(argv[i], &oid);
			oid_array_append(&bases, &oid);
		}
	}

	/* Give up if this is a baseless merge. */
	if (bases.nr == 0)
		return 2;

	return merge_resolve(&bases, p_head, p_remote);
}
