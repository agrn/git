/*
 * Builtin "git merge-one-file"
 *
 * Copyright (c) 2020 Alban Gruin
 *
 * Based on git-merge-one-file.sh, written by Linus Torvalds.
 *
 * This is the git per-file merge script, called with
 *
 *   $1 - original file SHA1 (or empty)
 *   $2 - file in branch1 SHA1 (or empty)
 *   $3 - file in branch2 SHA1 (or empty)
 *   $4 - pathname in repository
 *   $5 - original file mode (or empty)
 *   $6 - file in branch1 mode (or empty)
 *   $7 - file in branch2 mode (or empty)
 *
 * Handle some trivial cases.. The _really_ trivial cases have
 * been handled already by git read-tree, but that one doesn't
 * do any merges that might change the tree layout.
 */

#define USE_THE_INDEX_COMPATIBILITY_MACROS
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
	unsigned int orig_mode = 0, our_mode = 0, their_mode = 0, ret;
	struct lock_file lock = LOCK_INIT;

	if (argc != 8)
		usage(builtin_merge_one_file_usage);

	if (read_cache() < 0)
		die("invalid index");

	hold_locked_index(&lock, LOCK_DIE_ON_ERROR);

	if (!get_oid(argv[1], &orig_blob)) {
		p_orig_blob = &orig_blob;
		orig_mode = strtol(argv[5], NULL, 8);
	}

	if (!get_oid(argv[2], &our_blob)) {
		p_our_blob = &our_blob;
		our_mode = strtol(argv[6], NULL, 8);
	}

	if (!get_oid(argv[3], &their_blob)) {
		p_their_blob = &their_blob;
		their_mode = strtol(argv[7], NULL, 8);
	}

	ret = merge_strategies_one_file(the_repository,
					p_orig_blob, p_our_blob, p_their_blob, argv[4],
					orig_mode, our_mode, their_mode);

	if (ret) {
		rollback_lock_file(&lock);
		return ret;
	}

	return write_locked_index(&the_index, &lock, COMMIT_LOCK);
}
