/*
 * Builtin "git merge-one-file"
 *
 * Copyright (c) 2020 Alban Gruin
 *
 * Based on git-merge-one-file.sh, written by Linus Torvalds.
 *
 * This is the git per-file merge utility, called with
 *
 *   argv[1] - original file SHA1 (or empty)
 *   argv[2] - file in branch1 SHA1 (or empty)
 *   argv[3] - file in branch2 SHA1 (or empty)
 *   argv[4] - pathname in repository
 *   argv[5] - original file mode (or empty)
 *   argv[6] - file in branch1 mode (or empty)
 *   argv[7] - file in branch2 mode (or empty)
 *
 * Handle some trivial cases. The _really_ trivial cases have been
 * handled already by git read-tree, but that one doesn't do any merges
 * that might change the tree layout.
 */

#include "cache.h"
#include "builtin.h"
#include "lockfile.h"
#include "merge-strategies.h"

static const char builtin_merge_one_file_usage[] =
	"git merge-one-file <orig blob> <our blob> <their blob> <path> "
	"<orig mode> <our mode> <their mode>\n\n"
	"Blob ids and modes should be empty for missing files.";

int cmd_merge_one_file(int argc, const char **argv, const char *prefix)
{
	struct object_id orig_blob, our_blob, their_blob,
		*p_orig_blob = NULL, *p_our_blob = NULL, *p_their_blob = NULL;
	unsigned int orig_mode = 0, our_mode = 0, their_mode = 0, ret = 0;
	struct lock_file lock = LOCK_INIT;

	if (argc != 8)
		usage(builtin_merge_one_file_usage);

	if (repo_read_index(the_repository) < 0)
		die("invalid index");

	repo_hold_locked_index(the_repository, &lock, LOCK_DIE_ON_ERROR);

	if (!get_oid(argv[1], &orig_blob)) {
		p_orig_blob = &orig_blob;
		orig_mode = strtol(argv[5], NULL, 8);

		if (!(S_ISREG(orig_mode) || S_ISDIR(orig_mode) || S_ISLNK(orig_mode)))
			ret |= error(_("invalid 'orig' mode: %o"), orig_mode);
	}

	if (!get_oid(argv[2], &our_blob)) {
		p_our_blob = &our_blob;
		our_mode = strtol(argv[6], NULL, 8);

		if (!(S_ISREG(our_mode) || S_ISDIR(our_mode) || S_ISLNK(our_mode)))
			ret |= error(_("invalid 'our' mode: %o"), our_mode);
	}

	if (!get_oid(argv[3], &their_blob)) {
		p_their_blob = &their_blob;
		their_mode = strtol(argv[7], NULL, 8);

		if (!(S_ISREG(their_mode) || S_ISDIR(their_mode) || S_ISLNK(their_mode)))
			ret = error(_("invalid 'their' mode: %o"), their_mode);
	}

	if (ret)
		return ret;

	ret = merge_strategies_one_file(the_repository,
					p_orig_blob, p_our_blob, p_their_blob, argv[4],
					orig_mode, our_mode, their_mode);

	if (ret) {
		rollback_lock_file(&lock);
		return ret;
	}

	return write_locked_index(the_repository->index, &lock, COMMIT_LOCK);
}
