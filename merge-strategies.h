#ifndef MERGE_STRATEGIES_H
#define MERGE_STRATEGIES_H

#include "object.h"

int merge_strategies_one_file(struct repository *r,
			      const struct object_id *orig_blob,
			      const struct object_id *our_blob,
			      const struct object_id *their_blob, const char *path,
			      unsigned int orig_mode, unsigned int our_mode,
			      unsigned int their_mode);

#endif /* MERGE_STRATEGIES_H */
