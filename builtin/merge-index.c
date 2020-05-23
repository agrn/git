#define USE_THE_INDEX_COMPATIBILITY_MACROS
#include "builtin.h"
#include "merge-strategies.h"

int cmd_merge_index(int argc, const char **argv, const char *prefix)
{
	int i, force_file = 0, err = 0, one_shot = 0, quiet = 0;
	const char *pgm;

	/* Without this we cannot rely on waitpid() to tell
	 * what happened to our children.
	 */
	signal(SIGCHLD, SIG_DFL);

	if (argc < 3)
		usage("git merge-index [-o] [-q] <merge-program> (-a | [--] [<filename>...])");

	read_cache();

	i = 1;
	if (!strcmp(argv[i], "-o")) {
		one_shot = 1;
		i++;
	}
	if (!strcmp(argv[i], "-q")) {
		quiet = 1;
		i++;
	}
	pgm = argv[i++];
	for (; i < argc; i++) {
		const char *arg = argv[i];
		if (!force_file && *arg == '-') {
			if (!strcmp(arg, "--")) {
				force_file = 1;
				continue;
			}
			if (!strcmp(arg, "-a")) {
				err |= merge_all_index(&the_index, one_shot, quiet,
						       merge_one_file_spawn, (void *)pgm);
				continue;
			}
			die("git merge-index: unknown option %s", arg);
		}
		err |= merge_index_path(&the_index, one_shot, quiet, arg,
					merge_one_file_spawn, (void *)pgm);
	}
	return err;
}
