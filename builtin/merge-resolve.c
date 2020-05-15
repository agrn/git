/* Resolve two trees, using enhanced multi-base read-tree.  Based on
 * git-merge-resolve.sh, which is Copyright (c) 2005 Linus Torvalds and
 * Junio C Hamano.  The rewritten version is Copyright (c) 2020 Alban
 * Gruin. */

#define USE_THE_INDEX_COMPATIBILITY_MACROS
#include "cache.h"
#include "builtin.h"
#include "run-command.h"

static int merge_resolve(struct oid_array *bases, const struct object_id *head,
			 const struct object_id *remote)
{
	int i;
	struct child_process cp_update = CHILD_PROCESS_INIT,
		cp_read = CHILD_PROCESS_INIT,
		cp_write = CHILD_PROCESS_INIT;

	cp_update.git_cmd = 1;
	argv_array_pushl(&cp_update.args, "update-index", "-q", "--refresh", NULL);
	run_command(&cp_update);

	cp_read.git_cmd = 1;
	argv_array_pushl(&cp_read.args, "read-tree", "-u", "-m", "--aggressive", NULL);

	for (i = 0; i < bases->nr; i++)
		argv_array_push(&cp_read.args, oid_to_hex(bases->oid + i));

	if (head)
		argv_array_push(&cp_read.args, oid_to_hex(head));
	if (remote)
		argv_array_push(&cp_read.args, oid_to_hex(remote));

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
	int i, sep_seen = 0;
	struct oid_array bases = OID_ARRAY_INIT;
	struct object_id head, remote,
		*p_head = NULL, *p_remote = NULL;

	if (argc < 5)
		usage(builtin_merge_resolve_usage);

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
