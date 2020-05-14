/* merge-octopus.c -- Resolve two or more trees.
 *
 * Copyright (c) 2020 Alban Gruin
 *
 * Original script (git-merge-octopus.sh) by Junio Hamano. */

#define USE_THE_INDEX_COMPATIBILITY_MACROS
#include "cache.h"
#include "builtin.h"
#include "commit-reach.h"
#include "lockfile.h"
#include "run-command.h"
#include "unpack-trees.h"

static char *get_pretty(struct object_id *oid)
{
	struct strbuf var = STRBUF_INIT;
	char *hex, *pretty;

	hex = oid_to_hex(oid);
	strbuf_addf(&var, "GITHEAD_%s", hex);

	pretty = getenv(var.buf);
	if (!pretty)
		pretty = xstrdup(hex);

	strbuf_release(&var);

	return pretty;
}

/* static int reset_tree(struct object_id **trees, int nr, int update, int reset) */
/* { */
/* 	int nr_trees = 1; */
/* 	struct unpack_trees_options opts; */
/* 	struct tree_desc t[MAX_UNPACK_TREES]; */
/* 	struct tree *tree; */
/* 	struct lock_file lock_file = LOCK_INIT; */

/* 	read_cache_preload(NULL); */
/* 	if (refresh_cache(REFRESH_QUIET)) */
/* 		return -1; */

/* 	hold_locked_index(&lock_file, LOCK_DIE_ON_ERROR); */

/* 	memset(&opts, 0, sizeof(opts)); */

/* 	tree = parse_tree_indirect(i_tree); */
/* 	if (parse_tree(tree)) */
/* 		return -1; */

/* 	init_tree_desc(t, tree->buffer, tree->size); */

/* 	opts.head_idx = 1; */
/* 	opts.src_index = &the_index; */
/* 	opts.dst_index = &the_index; */
/* 	opts.merge = 1; */
/* 	opts.reset = reset; */
/* 	opts.update = update; */
/* 	opts.fn = oneway_merge; */

/* 	if (unpack_trees(nr_trees, t, &opts)) */
/* 		return -1; */

/* 	if (write_locked_index(&the_index, &lock_file, COMMIT_LOCK)) */
/* 		return error(_("unable to write new index file")); */

/* 	return 0; */
/* } */

static int write_tree(struct tree **reference_tree)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	struct strbuf read_tree = STRBUF_INIT, err = STRBUF_INIT;
	struct object_id oid;
	int ret;

	cp.git_cmd = 1;
	argv_array_push(&cp.args, "write-tree");
	ret = pipe_command(&cp, NULL, 0, &read_tree, 0, &err, 0);
	if (err.len > 0)
		fputs(err.buf, stderr);

	strbuf_trim_trailing_newline(&read_tree);
	get_oid(read_tree.buf, &oid);

	*reference_tree = lookup_tree(the_repository, &oid);

	strbuf_release(&read_tree);
	strbuf_release(&err);

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
		char *pretty_name;
		int can_ff = 1;

		if (ret) {
			puts(_("Automated merge did not work."));
			puts(_("Should not be doing an octopus."));

			ret = 2;
			goto out;
		}

		pretty_name = get_pretty(oid);
		c = lookup_commit_reference(the_repository, oid);
		common = get_merge_bases_many_dirty(c, references, reference_commit);

		if (commit_list_count(common) == 0)
			die(_("Unable to find common commit with %s"), pretty_name);

		for (l = common; l && !oideq(&l->item->object.oid, oid); l = l->next);

		if (l) {
			printf(_("Already up to date with %s\n"), pretty_name);
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
			struct child_process cp = CHILD_PROCESS_INIT;

			printf(_("Fast-forwarding to: %s\n"), pretty_name);

			cp.git_cmd = 1;
			argv_array_pushl(&cp.args, "read-tree", "-u", "-m", NULL);
			argv_array_push(&cp.args, oid_to_hex(head));
			argv_array_push(&cp.args, oid_to_hex(oid));

			ret = run_command(&cp);
			if (ret)
				goto out;

			references = 1;
			write_tree(&reference_tree);
		} else {
			struct tree *next = NULL;
			struct child_process cp = CHILD_PROCESS_INIT;
			

			non_ff_merge = 1;
			printf(_("Trying simple merge with %s\n"), pretty_name);

			cp.git_cmd = 1;
			argv_array_pushl(&cp.args, "read-tree", "-u", "-m", "--aggressive", NULL);

			for (l = common; l; l = l->next)
				argv_array_push(&cp.args, oid_to_hex(&l->item->object.oid));

			argv_array_push(&cp.args, oid_to_hex(&reference_tree->object.oid));
			argv_array_push(&cp.args, oid_to_hex(oid));

			if (run_command(&cp)) {
				ret = 2;
				goto out;
			}

			if (write_tree(&next)) {
				struct child_process cp = CHILD_PROCESS_INIT;
				puts(_("Simple merge did not work, trying automatic merge."));

				cp.git_cmd = 1;
				argv_array_pushl(&cp.args, "merge-index", "-o",
						 "git-merge-one-file", "-a", NULL);
				if (run_command(&cp))
					ret = 1;

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

int cmd_merge_octopus(int argc, const char **argv, const char *prefix)
{
	int i, sep_seen = 0;
	struct oid_array bases = OID_ARRAY_INIT, remotes = OID_ARRAY_INIT;
	struct object_id head, *p_head = NULL;

	/* The first parameters up to -- are merge bases; the rest are
	 * heads. */
	for (i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--") == 0)
			sep_seen = 1;
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

	return merge_octopus(&bases, p_head, &remotes);
}
