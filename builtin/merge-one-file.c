#define USE_THE_INDEX_COMPATIBILITY_MACROS
#include "cache.h"
#include "builtin.h"
#include "commit.h"
#include "dir.h"
#include "lockfile.h"
#include "object-store.h"
#include "run-command.h"
#include "xdiff-interface.h"

static int add_to_index_cacheinfo(unsigned int mode,
				  const struct object_id *oid, const char *path)
{
	struct child_process cp = CHILD_PROCESS_INIT;

	cp.git_cmd = 1;
	argv_array_pushl(&cp.args, "update-index", "--add", "--cacheinfo", NULL);
	argv_array_pushf(&cp.args, "%o,%s,%s", mode, oid_to_hex(oid), path);
	return run_command(&cp);
}

static int remove_from_index(const char *path)
{
	struct child_process cp = CHILD_PROCESS_INIT;

	cp.git_cmd = 1;
	argv_array_pushl(&cp.args, "update-index", "--remove", "--", path, NULL);
	return run_command(&cp);
}

static int checkout_from_index(const char *path)
{
	struct child_process cp = CHILD_PROCESS_INIT;

	cp.git_cmd = 1;
	argv_array_pushl(&cp.args, "checkout-index", "-u", "-f", "--", path, NULL);
	return run_command(&cp);
}

static int merge_one_file_deleted(const struct object_id *orig_blob,
				  const struct object_id *our_blob,
				  const struct object_id *their_blob, const char *path,
				  unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode)
{
	if ((our_blob && orig_mode != our_mode) ||
	    (their_blob && orig_mode != their_mode))
		return error(_("File %s deleted on one branch but had its "
			       "permissions changed on the other."), path);

	if (our_blob) {
		printf("Removing %s\n", path);

		if (file_exists(path))
			remove_path(path);
	}

	return remove_from_index(path);
}

static int do_merge_one_file(const struct object_id *orig_blob,
			     const struct object_id *our_blob,
			     const struct object_id *their_blob, const char *path,
			     unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode)
{
	int ret, i, dest;
	mmbuffer_t result = {NULL, 0};
	mmfile_t mmfs[3];
	xmparam_t xmp = {{0}};
	struct lock_file lock = LOCK_INIT;
	struct cache_entry *ce;

	if (read_cache() < 0)
		die("corrupted cache");

	hold_locked_index(&lock, LOCK_DIE_ON_ERROR);

	if (our_mode == S_IFLNK || their_mode == S_IFLNK)
		return error(_("%s: Not merging symbolic link changes."), path);
	else if (our_mode == S_IFGITLINK || their_mode == S_IFGITLINK)
		return error(_("%s: Not merging conflicting submodule changes."), path);

	read_mmblob(mmfs + 0, our_blob);
	read_mmblob(mmfs + 2, their_blob);

	if (orig_blob) {
		printf("Auto-merging %s\n", path);
		read_mmblob(mmfs + 1, orig_blob);
	} else {
		printf("Added %s in both, but differently.\n", path);
		read_mmblob(mmfs + 1, the_hash_algo->empty_blob);
	}

	xmp.level = XDL_MERGE_ZEALOUS_ALNUM;
	xmp.style = 0;
	xmp.favor = 0;

	ret = xdl_merge(mmfs + 1, mmfs + 0, mmfs + 2, &xmp, &result);

	for (i = 0; i < 3; i++)
		free(mmfs[i].ptr);

	if (ret > 127)
		ret = 1;

	ce = cache_file_exists(path, strlen(path), 0);
	if (!ce)
		BUG("file is not present in the cache?");

	unlink(path);
	dest = open(path, O_WRONLY | O_CREAT, ce->ce_mode);
	write_in_full(dest, result.ptr, result.size);
	close(dest);

	free(result.ptr);

	if (ret) {
		if (!orig_blob)
			error(_("content conflict in %s"), path);
		if (our_mode != their_mode)
			error(_("permission conflict: %o->%o,%o in %s"),
			      orig_mode, our_mode, their_mode, path);

		return 1;
	}

	add_file_to_cache(path, 0);
	return write_locked_index(&the_index, &lock, COMMIT_LOCK);
}

static int merge_one_file(const struct object_id *orig_blob,
			  const struct object_id *our_blob,
			  const struct object_id *their_blob, const char *path,
			  unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode)
{
	if (orig_blob &&
	    ((our_blob && oideq(orig_blob, our_blob)) ||
	     (their_blob && oideq(orig_blob, their_blob))))
		return merge_one_file_deleted(orig_blob, our_blob, their_blob, path,
					      orig_mode, our_mode, their_mode);
	else if (!orig_blob && our_blob && !their_blob) {
		return add_to_index_cacheinfo(our_mode, our_blob, path);
	} else if (!orig_blob && !our_blob && their_blob) {
		printf("Adding %s\n", path);

		if (file_exists(path))
			return error(_("untracked %s is overwritten by the merge.\n"), path);

		if (add_to_index_cacheinfo(their_mode, their_blob, path))
			return 1;
		return checkout_from_index(path);
	} else if (!orig_blob && our_blob && their_blob &&
		   oideq(our_blob, their_blob)) {
		if (our_mode != their_mode)
			return error(_("File %s added identically in both branches, "
				       "but permissions conflict %o->%o."),
				     path, our_mode, their_mode);

		printf("Adding %s\n", path);

		if (add_to_index_cacheinfo(our_mode, our_blob, path))
			return 1;
		return checkout_from_index(path);
	} else if (our_blob && their_blob)
		return do_merge_one_file(orig_blob, our_blob, their_blob, path,
					 orig_mode, our_mode, their_mode);
	else {
		char *orig_hex = "", *our_hex = "", *their_hex = "";

		if (orig_blob)
			orig_hex = oid_to_hex(orig_blob);
		if (our_blob)
			our_hex = oid_to_hex(our_blob);
		if (their_blob)
			their_hex = oid_to_hex(their_blob);

		return error(_("%s: Not handling case %s -> %s -> %s"),
			path, orig_hex, our_hex, their_hex);
	}

	return 0;
}

static const char builtin_merge_one_file_usage[] =
	"git merge-one-file <orig blob> <our blob> <their blob> <path> "
	"<orig mode> <our mode> <their mode>\n\n"
	"Blob ids and modes should be empty for missing files.";

int cmd_merge_one_file(int argc, const char **argv, const char *prefix)
{
	struct object_id orig_blob, our_blob, their_blob,
		*p_orig_blob = NULL, *p_our_blob = NULL, *p_their_blob = NULL;
	unsigned int orig_mode = 0, our_mode = 0, their_mode = 0;

	if (argc != 8)
		usage(builtin_merge_one_file_usage);

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

	return merge_one_file(p_orig_blob, p_our_blob, p_their_blob, argv[4],
			      orig_mode, our_mode, their_mode);
}
