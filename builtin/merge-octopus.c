/* merge-octopus.c -- Resolve two or more trees.
 *
 * Copyright (c) 2020 Alban Gruin
 *
 * Original script (git-merge-octopus.sh) by Junio Hamano. */

#include "cache.h"
#include "cache-tree.h"
#include "builtin.h"
#include "commit-reach.h"
#include "lockfile.h"
#include "merge-strategies.h"
#include "unpack-trees.h"

static int fast_forward(const struct object_id *oids, int nr, int aggressive)
{
	int i;
	struct tree_desc t[MAX_UNPACK_TREES];
	struct unpack_trees_options opts;
	struct lock_file lock = LOCK_INIT;

	repo_read_index_preload(the_repository, NULL, 0);
	if (refresh_index(the_repository->index, REFRESH_QUIET, NULL, NULL, NULL))
		return -1;

	repo_hold_locked_index(the_repository, &lock, LOCK_DIE_ON_ERROR);

	memset(&opts, 0, sizeof(opts));
	opts.head_idx = 1;
	opts.src_index = the_repository->index;
	opts.dst_index = the_repository->index;
	opts.merge = 1;
	opts.update = 1;
	opts.aggressive = aggressive;

	for (i = 0; i < nr; i++) {
		struct tree *tree;
		tree = parse_tree_indirect(oids + i);
		if (parse_tree(tree))
			return -1;
		init_tree_desc(t + i, tree->buffer, tree->size);
	}

	if (nr == 1)
		opts.fn = oneway_merge;
	else if (nr == 2) {
		opts.fn = twoway_merge;
		opts.initial_checkout = is_index_unborn(the_repository->index);
	} else if (nr >= 3) {
		opts.fn = threeway_merge;
		opts.head_idx = nr - 1;
	}

	if (unpack_trees(nr, t, &opts))
		return -1;

	if (write_locked_index(the_repository->index, &lock, COMMIT_LOCK))
		return error(_("unable to write new index file"));

	return 0;
}

static int write_tree(struct tree **reference_tree)
{
	struct object_id oid;
	int ret;

	ret = write_index_as_tree(&oid, the_repository->index,
				  the_repository->index_file, 0, NULL);
	if (!ret)
		*reference_tree = lookup_tree(the_repository, &oid);

	return ret;
}

static int merge_octopus(struct commit_list *bases, const char *head_arg,
			 struct commit_list *remotes)
{
	int non_ff_merge = 0, ret = 0, references = 1;
	struct commit **reference_commit;
	struct tree *reference_tree;
	struct commit_list *j;
	struct object_id head;
	struct strbuf sb = STRBUF_INIT;

	get_oid(head_arg, &head);

	reference_commit = xcalloc(commit_list_count(remotes) + 1, sizeof(struct commit *));
	reference_commit[0] = lookup_commit_reference(the_repository, &head);

	if (repo_index_has_changes(the_repository,
				   repo_get_commit_tree(the_repository, reference_commit[0]),
				   &sb)) {
		error(_("Your local changes to the following files "
			"would be overwritten by merge:\n  %s"),
		      sb.buf);
		strbuf_release(&sb);
		return 2;
	}

	write_tree(&reference_tree);

	for (j = remotes; j; j = j->next) {
		struct commit *c = j->item;
		struct object_id *oid = &c->object.oid;
		struct commit_list *common, *k;
		char *branch_name;
		int can_ff = 1;

		if (ret) {
			puts(_("Automated merge did not work."));
			puts(_("Should not be doing an octopus."));

			ret = 2;
			goto out;
		}

		branch_name = merge_get_better_branch_name(oid_to_hex(oid));
		common = get_merge_bases_many(c, references, reference_commit);

		if (!common)
			die(_("Unable to find common commit with %s"), branch_name);

		for (k = common; k && !oideq(&k->item->object.oid, oid); k = k->next);

		if (k) {
			printf(_("Already up to date with %s\n"), branch_name);
			free(branch_name);
			free_commit_list(common);
			continue;
		}

		if (!non_ff_merge) {
			int i;

			for (i = 0, k = common; k && i < references && can_ff; k = k->next, i++) {
				can_ff = oideq(&k->item->object.oid,
					       &reference_commit[i]->object.oid);
			}
		}

		if (!non_ff_merge && can_ff) {
			struct object_id oids[2];
			printf(_("Fast-forwarding to: %s\n"), branch_name);

			oidcpy(oids, &head);
			oidcpy(oids + 1, oid);

			ret = fast_forward(oids, 2, 0);
			if (ret) {
				free(branch_name);
				free_commit_list(common);
				goto out;
			}

			references = 0;
			write_tree(&reference_tree);
		} else {
			int i = 0;
			struct tree *next = NULL;
			struct object_id oids[MAX_UNPACK_TREES];

			non_ff_merge = 1;
			printf(_("Trying simple merge with %s\n"), branch_name);

			for (k = common; k; k = k->next)
				oidcpy(oids + (i++), &k->item->object.oid);

			oidcpy(oids + (i++), &reference_tree->object.oid);
			oidcpy(oids + (i++), oid);

			if (fast_forward(oids, i, 1)) {
				ret = 2;

				free(branch_name);
				free_commit_list(common);

				goto out;
			}

			if (write_tree(&next)) {
				struct lock_file lock = LOCK_INIT;

				puts(_("Simple merge did not work, trying automatic merge."));
				repo_hold_locked_index(the_repository, &lock, LOCK_DIE_ON_ERROR);
				ret = !!merge_all(the_repository->index, 0, 0,
						  merge_one_file_cb, the_repository);
				write_locked_index(the_repository->index, &lock, COMMIT_LOCK);

				write_tree(&next);
			}

			reference_tree = next;
		}

		reference_commit[references++] = c;

		free(branch_name);
		free_commit_list(common);
	}

out:
	free(reference_commit);
	return ret;
}

static const char builtin_merge_octopus_usage[] =
	"git merge-octopus [<bases>...] -- <head> <remote1> <remote2> [<remotes>...]";

int cmd_merge_octopus(int argc, const char **argv, const char *prefix)
{
	int i, sep_seen = 0;
	struct commit_list *bases = NULL, *remotes = NULL;
	struct commit_list **next_base = &bases, **next_remote = &remotes;
	const char *head_arg = NULL;

	if (argc < 5)
		usage(builtin_merge_octopus_usage);

	setup_work_tree();
	if (repo_read_index(the_repository) < 0)
		die("corrupted cache");

	/* The first parameters up to -- are merge bases; the rest are
	 * heads. */
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--") == 0)
			sep_seen = 1;
		else if (strcmp(argv[i], "-h") == 0)
			usage(builtin_merge_octopus_usage);
		else if (sep_seen && !head_arg)
			head_arg = argv[i];
		else {
			struct object_id oid;

			get_oid(argv[i], &oid);

			if (!oideq(&oid, the_hash_algo->empty_tree)) {
				struct commit *commit;
				commit = lookup_commit_or_die(&oid, argv[i]);

				if (sep_seen)
					next_remote = commit_list_append(commit, next_remote);
				else
					next_base = commit_list_append(commit, next_base);
			}
		}
	}

	/* Reject if this is not an octopus -- resolve should be used
	 * instead. */
	if (commit_list_count(remotes) < 2)
		return 2;

	return merge_octopus(bases, head_arg, remotes);
}
