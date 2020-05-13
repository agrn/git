#define USE_THE_INDEX_COMPATIBILITY_MACROS
#include "cache.h"
#include "builtin.h"
#include "commit.h"
#include "dir.h"
#include "lockfile.h"
#include "object-store.h"
#include "xdiff-interface.h"

static int add_to_index_cacheinfo(unsigned int mode,
				  const struct object_id *oid, const char *path)
{
	struct cache_entry *ce;
	int len, option;

	if (!verify_path(path, mode))
		return error("Invalid path '%s'", path);

	len = strlen(path);
	ce = make_empty_cache_entry(&the_index, len);

	oidcpy(&ce->oid, oid);
	memcpy(ce->name, path, len);
	ce->ce_flags = create_ce_flags(0);
	ce->ce_namelen = len;
	ce->ce_mode = create_ce_mode(mode);
	if (assume_unchanged)
		ce->ce_flags |= CE_VALID;
	option = ADD_CACHE_OK_TO_ADD | ADD_CACHE_OK_TO_REPLACE;
	if (add_cache_entry(ce, option))
		return error("%s: cannot add to the index", path);

	return 0;
}

static int checkout_from_index(const char *path)
{
	struct checkout state;
	struct cache_entry *ce;

	state.istate = &the_index;
	state.force = 1;
	state.base_dir = "";
	state.base_dir_len = 0;

	ce = cache_file_exists(path, strlen(path), 0);
	if (checkout_entry(ce, &state, NULL, NULL) < 0)
		return error("%s: cannot checkout file", path);
	return 0;
}

static int merge_one_file_deleted(const struct object_id *orig_blob,
				  const struct object_id *our_blob,
				  const struct object_id *their_blob, const char *path,
				  unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode)
{
	if ((our_blob && orig_mode != our_mode) ||
	    (their_blob && orig_mode != their_mode)) {
		fprintf(stderr, "ERROR: File %s deleted on one branch but had its\n", path);
		fprintf(stderr, "ERROR: permissions changed on the other.\n");
		return 1;
	}

	if (our_blob) {
		printf("Removing %s\n", path);

		if (file_exists(path))
			remove_path(path);
	}

	if (remove_file_from_cache(path))
		return error("%s: cannot remove from the index", path);
	return 0;
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
	struct cache_entry *ce;

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
		fprintf(stderr, "ERROR: ");

		if (!orig_blob) {
			fprintf(stderr, "content conflict");
			if (our_mode != their_mode)
				fprintf(stderr, ", ");
		}

		if (our_mode != their_mode)
			fprintf(stderr, "permissions conflict: %o->%o,%o",
				orig_mode, our_mode, their_mode);

		fprintf(stderr, " in %s\n", path);

		return 1;
	}

	return add_file_to_cache(path, 0);
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

		if (file_exists(path)) {
			fprintf(stderr, "ERROR: untracked %s is overwritten by the merge.\n", path);
			return 1;
		}

		if (add_to_index_cacheinfo(their_mode, their_blob, path))
			return 1;
		return checkout_from_index(path);
	} else if (!orig_blob && our_blob && their_blob &&
		   oideq(our_blob, their_blob)) {
		if (our_mode != their_mode) {
			fprintf(stderr, "ERROR: File %s added identically in both branches,", path);
			fprintf(stderr, "ERROR: but permissions conflict %o->%o.\n",
				our_mode, their_mode);
			return 1;
		}

		printf("Adding %s\n", path);

		if (add_to_index_cacheinfo(our_mode, our_blob, path))
			return 1;
		return checkout_from_index(path);
	} else if (our_blob && their_blob)
		return do_merge_one_file(orig_blob, our_blob, their_blob, path,
					 orig_mode, our_mode, their_mode);
	else {
		fprintf(stderr, "ERROR: %s: Not handling case %s -> %s -> %s\n",
			path, oid_to_hex(orig_blob), oid_to_hex(our_blob), oid_to_hex(their_blob));
		return 1;
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

	ret = merge_one_file(p_orig_blob, p_our_blob, p_their_blob, argv[4],
			     orig_mode, our_mode, their_mode);

	if (ret) {
		rollback_lock_file(&lock);
		return ret;
	}

	return write_locked_index(&the_index, &lock, COMMIT_LOCK);
}
