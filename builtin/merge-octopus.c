/*
 * Builtin "git merge-octopus"
 *
 * Copyright (c) 2020 Alban Gruin
 *
 * Based on git-merge-octopus.sh, written by Junio C Hamano.
 *
 * Resolve two or more trees.
 */

#include "cache.h"
#include "builtin.h"
#include "commit-reach.h"
#include "lockfile.h"
#include "run-command.h"
#include "unpack-trees.h"

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
	child_process_clear(&cp);

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

	get_oid(head_arg, &head);
	reference_commit = xcalloc(commit_list_count(remotes) + 1, sizeof(struct commit *));
	reference_commit[0] = lookup_commit_reference(the_repository, &head);
	reference_tree = get_commit_tree(reference_commit[0]);

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
			struct child_process cp = CHILD_PROCESS_INIT;

			printf(_("Fast-forwarding to: %s\n"), branch_name);

			cp.git_cmd = 1;
			argv_array_pushl(&cp.args, "read-tree", "-u", "-m", NULL);
			argv_array_push(&cp.args, oid_to_hex(&head));
			argv_array_push(&cp.args, oid_to_hex(oid));

			ret = run_command(&cp);
			if (ret) {
				free(branch_name);
				free_commit_list(common);
				goto out;
			}

			child_process_clear(&cp);
			references = 0;
			write_tree(&reference_tree);
		} else {
			struct commit_list *l;
			struct tree *next = NULL;
			struct child_process cp = CHILD_PROCESS_INIT;

			non_ff_merge = 1;
			printf(_("Trying simple merge with %s\n"), branch_name);

			cp.git_cmd = 1;
			argv_array_pushl(&cp.args, "read-tree", "-u", "-m", "--aggressive", NULL);

			for (l = common; l; l = l->next)
				argv_array_push(&cp.args, oid_to_hex(&l->item->object.oid));

			argv_array_push(&cp.args, oid_to_hex(&reference_tree->object.oid));
			argv_array_push(&cp.args, oid_to_hex(oid));

			if (run_command(&cp)) {
				ret = 2;

				free(branch_name);
				free_commit_list(common);

				goto out;
			}

			child_process_clear(&cp);

			if (write_tree(&next)) {
				struct child_process cp = CHILD_PROCESS_INIT;
				puts(_("Simple merge did not work, trying automatic merge."));

				cp.git_cmd = 1;
				argv_array_pushl(&cp.args, "merge-index", "-o",
						 "git-merge-one-file", "-a", NULL);
				if (run_command(&cp))
					ret = 1;

				child_process_clear(&cp);
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
	struct child_process cp = CHILD_PROCESS_INIT;
	struct strbuf files = STRBUF_INIT;

	if (argc < 5)
		usage(builtin_merge_octopus_usage);

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

	return merge_octopus(bases, head_arg, remotes);
}
