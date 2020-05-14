/* Resolve two trees, using enhanced multi-base read-tree.  Based on
 * git-merge-resolve.sh, which is Copyright (c) 2005 Linus Torvalds and
 * Junio C Hamano.  The rewritten version is Copyright (c) 2020 Alban
 * Gruin. */

#include "cache.h"
#include "builtin.h"
#include "run-command.h"

static int merge_resolve(struct commit_list *bases, const char *head_arg,
			 struct commit_list *remote)
{
	struct commit_list *j;
	struct child_process cp_update = CHILD_PROCESS_INIT,
		cp_read = CHILD_PROCESS_INIT,
		cp_write = CHILD_PROCESS_INIT;

	cp_update.git_cmd = 1;
	argv_array_pushl(&cp_update.args, "update-index", "-q", "--refresh", NULL);
	run_command(&cp_update);

	cp_read.git_cmd = 1;
	argv_array_pushl(&cp_read.args, "read-tree", "-u", "-m", "--aggressive", NULL);

	for (j = bases; j && j->item; j = j->next)
		argv_array_push(&cp_read.args, oid_to_hex(&j->item->object.oid));

	if (head_arg)
		argv_array_push(&cp_read.args, head_arg);
	if (remote && remote->item)
		argv_array_push(&cp_read.args, oid_to_hex(&remote->item->object.oid));

	if (run_command(&cp_read))
		return 2;

	puts("Trying simple merge.");

	cp_write.git_cmd = 1;
	cp_write.no_stdout = 1;
	cp_write.no_stderr = 1;
	argv_array_push(&cp_write.args, "write-tree");
	if (run_command(&cp_write)) {
		struct child_process cp_merge = CHILD_PROCESS_INIT;

		puts("Simple merge failed, trying Automatic merge.");

		cp_merge.git_cmd = 1;
		argv_array_pushl(&cp_merge.args, "merge-index", "-o",
				 "git-merge-one-file", "-a", NULL);
		if (run_command(&cp_merge))
			return 1;
	}

	return 0;
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
