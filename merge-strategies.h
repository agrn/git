#ifndef MERGE_STRATEGIES_H
#define MERGE_STRATEGIES_H

#include "object.h"

int merge_three_way(struct index_state *istate,
		    const struct object_id *orig_blob,
		    const struct object_id *our_blob,
		    const struct object_id *their_blob, const char *path,
		    unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode);

typedef int (*merge_fn)(struct index_state *istate,
			const struct object_id *orig_blob,
			const struct object_id *our_blob,
			const struct object_id *their_blob, const char *path,
			unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode,
			void *data);

int merge_one_file_func(struct index_state *istate,
			const struct object_id *orig_blob,
			const struct object_id *our_blob,
			const struct object_id *their_blob, const char *path,
			unsigned int orig_mode, unsigned int our_mode, unsigned int their_mode,
			void *data);

int merge_index_path(struct index_state *istate, int oneshot, int quiet,
		     const char *path, merge_fn fn, void *data);
int merge_all_index(struct index_state *istate, int oneshot, int quiet,
		    merge_fn fn, void *data);

#endif /* MERGE_STRATEGIES_H */
