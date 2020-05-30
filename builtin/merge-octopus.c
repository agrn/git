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
#include "run-command.h"
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

static int merge_octopus(struct oid_array *bases, const struct object_id *head,
			 struct oid_array *remotes)
{
	int i, non_ff_merge = 0, ret = 0, references = 1;
	struct commit **reference_commit;
	struct tree *reference_tree;

	reference_commit = xcalloc(remotes->nr + 1, sizeof(struct commit *));
	reference_commit[0] = lookup_commit_reference(the_repository, head);
	write_tree(&reference_tree);

	for (i = 0; i < remotes->nr; i++) {
		struct object_id *oid = remotes->oid + i;
		struct commit *c;
		struct commit_list *common, *l;
		char *branch_name;
		int can_ff = 1;

		if (ret) {
			puts(_("Automated merge did not work."));
			puts(_("Should not be doing an octopus."));

			ret = 2;
			goto out;
		}

		branch_name = merge_get_better_branch_name(oid_to_hex(oid));
		c = lookup_commit_reference(the_repository, oid);
		common = get_merge_bases_many(c, references, reference_commit);

		if (!common)
			die(_("Unable to find common commit with %s"), branch_name);

		for (l = common; l && !oideq(&l->item->object.oid, oid); l = l->next);

		if (l) {
			printf(_("Already up to date with %s\n"), branch_name);
			free_commit_list(common);
			continue;
		}

		if (!non_ff_merge) {
			int j;

			for (j = 0, l = common; l && j < references && can_ff; l = l->next, j++) {
				can_ff = oideq(&l->item->object.oid,
					       &reference_commit[j]->object.oid);
			}
		}

		if (!non_ff_merge && can_ff) {
			struct object_id oids[2];
			printf(_("Fast-forwarding to: %s\n"), branch_name);

			oidcpy(oids, head);
			oidcpy(oids + 1, oid);

			ret = fast_forward(oids, 2, 0);
			if (ret)
				goto out;

			references = 0;
			write_tree(&reference_tree);
		} else {
			int j = 0;
			struct tree *next = NULL;
			struct object_id oids[MAX_UNPACK_TREES];

			non_ff_merge = 1;
			printf(_("Trying simple merge with %s\n"), branch_name);

			for (l = common; l; l = l->next)
				oidcpy(oids + (j++), &l->item->object.oid);

			oidcpy(oids + (j++), &reference_tree->object.oid);
			oidcpy(oids + (j++), oid);

			if (fast_forward(oids, j, 1)) {
				ret = 2;
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
	struct oid_array bases = OID_ARRAY_INIT, remotes = OID_ARRAY_INIT;
	struct object_id head, *p_head = NULL;
	struct child_process cp = CHILD_PROCESS_INIT;
	struct strbuf files = STRBUF_INIT;

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
		else if (sep_seen && !p_head) {
			if (!get_oid(argv[i], &head))
				p_head = &head;
		} else {
			struct object_id oid;

			get_oid(argv[i], &oid);
			if (sep_seen)
				oid_array_append(&remotes, &oid);
			else
				oid_array_append(&bases, &oid);
		}
	}

	/* Reject if this is not an octopus -- resolve should be used
	 * instead. */
	if (remotes.nr < 2)
		return 2;

	cp.git_cmd = 1;
	argv_array_pushl(&cp.args, "diff-index", "--cached",
			 "--name-only", "HEAD", "--", NULL);
	pipe_command(&cp, NULL, 0, &files, 0, NULL, 0);
	child_process_clear(&cp);

	if (files.len > 0) {
		struct strbuf **s, **b;

		s = strbuf_split(&files, '\n');

		fprintf(stderr, _("Error: Your local changes to the following "
				  "files would be overwritten by merge\n"));

		for (b = s; *b; b++)
			fprintf(stderr, "    %.*s", (int)(*b)->len, (*b)->buf);

		strbuf_list_free(s);
		strbuf_release(&files);
		return 2;
	}

	return merge_octopus(&bases, p_head, &remotes);
}
