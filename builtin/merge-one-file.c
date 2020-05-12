#define USE_THE_INDEX_COMPATIBILITY_MACROS
#include "cache.h"
#include "builtin.h"
#include "commit.h"
#include "dir.h"
#include "lockfile.h"
#include "object-store.h"
#include "run-command.h"
#include "xdiff-interface.h"

static int create_temp_file(const struct object_id *oid, struct strbuf *path)
{
	struct child_process cp = CHILD_PROCESS_INIT;
	struct strbuf err = STRBUF_INIT;
	int ret;

	cp.git_cmd = 1;
	argv_array_pushl(&cp.args, "unpack-file", oid_to_hex(oid), NULL);
	ret = pipe_command(&cp, NULL, 0, path, 0, &err, 0);
	if (!ret && path->len > 0)
		strbuf_trim_trailing_newline(path);

	fprintf(stderr, "%.*s", (int) err.len, err.buf);
	strbuf_release(&err);

	return ret;
}

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

	return remove_from_index(path);
}

static int do_merge_one_file(const struct object_id *orig_blob,
			     const struct object_id *our_blob,
			     const struct object_id *their_blob, const char *path,
			     unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode)
{
	int ret, source, dest;
	struct strbuf src1 = STRBUF_INIT, src2 = STRBUF_INIT, orig = STRBUF_INIT;
	struct child_process cp_merge = CHILD_PROCESS_INIT,
		cp_checkout = CHILD_PROCESS_INIT,
		cp_update = CHILD_PROCESS_INIT;

	if (our_mode == S_IFLNK || their_mode == S_IFLNK) {
		fprintf(stderr, "ERROR: %s: Not merging symbolic link changes.\n", path);
		return 1;
	} else if (our_mode == S_IFGITLINK || their_mode == S_IFGITLINK) {
		fprintf(stderr, "ERROR: %s: Not merging conflicting submodule changes.\n",
			path);
		return 1;
	}

	create_temp_file(our_blob, &src1);
	create_temp_file(their_blob, &src2);

	if (orig_blob) {
		printf("Auto-merging %s\n", path);
		create_temp_file(orig_blob, &orig);
	} else {
		printf("Added %s in both, but differently.\n", path);
		create_temp_file(the_hash_algo->empty_blob, &orig);
	}

	cp_merge.git_cmd = 1;
	argv_array_pushl(&cp_merge.args, "merge-file", src1.buf, orig.buf, src2.buf,
			 NULL);
	ret = run_command(&cp_merge);

	if (ret != 0)
		ret = 1;

	cp_checkout.git_cmd = 1;
	argv_array_pushl(&cp_checkout.args, "checkout-index", "-f", "--stage=2",
			 "--", path, NULL);
	if (run_command(&cp_checkout))
		return 1;

	source = open(src1.buf, O_RDONLY);
	dest = open(path, O_WRONLY | O_TRUNC);

	copy_fd(source, dest);

	close(source);
	close(dest);

	unlink(orig.buf);
	unlink(src1.buf);
	unlink(src2.buf);

	strbuf_release(&src1);
	strbuf_release(&src2);
	strbuf_release(&orig);

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

	cp_update.git_cmd = 1;
	argv_array_pushl(&cp_update.args, "update-index", "--", path, NULL);
	return run_command(&cp_update);
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
