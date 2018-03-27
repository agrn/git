#include "cache.h"
#include "object.h"
#include "pack.h"
#include "pack-objects.h"
#include "packfile.h"

static uint32_t locate_object_entry_hash(struct packing_data *pdata,
					 const unsigned char *sha1,
					 int *found)
{
	uint32_t i, mask = (pdata->index_size - 1);

	i = sha1hash(sha1) & mask;

	while (pdata->index[i] > 0) {
		uint32_t pos = pdata->index[i] - 1;

		if (!hashcmp(sha1, pdata->objects[pos].idx.oid.hash)) {
			*found = 1;
			return i;
		}

		i = (i + 1) & mask;
	}

	*found = 0;
	return i;
}

static inline uint32_t closest_pow2(uint32_t v)
{
	v = v - 1;
	v |= v >> 1;
	v |= v >> 2;
	v |= v >> 4;
	v |= v >> 8;
	v |= v >> 16;
	return v + 1;
}

static void rehash_objects(struct packing_data *pdata)
{
	uint32_t i;
	struct object_entry *entry;

	pdata->index_size = closest_pow2(pdata->nr_objects * 3);
	if (pdata->index_size < 1024)
		pdata->index_size = 1024;

	free(pdata->index);
	pdata->index = xcalloc(pdata->index_size, sizeof(*pdata->index));

	entry = pdata->objects;

	for (i = 0; i < pdata->nr_objects; i++) {
		int found;
		uint32_t ix = locate_object_entry_hash(pdata,
						       entry->idx.oid.hash,
						       &found);

		if (found)
			die("BUG: Duplicate object in hash");

		pdata->index[ix] = i + 1;
		entry++;
	}
}

struct object_entry *packlist_find(struct packing_data *pdata,
				   const unsigned char *sha1,
				   uint32_t *index_pos)
{
	uint32_t i;
	int found;

	if (!pdata->index_size)
		return NULL;

	i = locate_object_entry_hash(pdata, sha1, &found);

	if (index_pos)
		*index_pos = i;

	if (!found)
		return NULL;

	return &pdata->objects[pdata->index[i] - 1];
}

static void prepare_in_pack_by_idx(struct packing_data *pdata)
{
	struct packed_git **mapping, *p;
	int cnt = 0, nr = 1 << OE_IN_PACK_BITS;

	if (getenv("GIT_TEST_FULL_IN_PACK_ARRAY")) {
		/*
		 * leave in_pack_by_idx NULL to force in_pack[] to be
		 * used instead
		 */
		return;
	}

	ALLOC_ARRAY(mapping, nr);
	mapping[cnt++] = NULL; /* zero index must be mapped to NULL */
	for (p = get_packed_git(the_repository); p; p = p->next, cnt++) {
		if (cnt == nr) {
			free(mapping);
			return;
		}
		p->index = cnt;
		mapping[cnt] = p;
	}
	pdata->in_pack_by_idx = mapping;
}

struct object_entry *packlist_alloc(struct packing_data *pdata,
				    const unsigned char *sha1,
				    uint32_t index_pos)
{
	struct object_entry *new_entry;

	if (!pdata->nr_objects) {
		prepare_in_pack_by_idx(pdata);
		if (getenv("GIT_TEST_OE_SIZE_BITS")) {
			int bits = atoi(getenv("GIT_TEST_OE_SIZE_BITS"));;
			pdata->oe_size_limit = 1 << bits;
		}
		if (!pdata->oe_size_limit)
			pdata->oe_size_limit = 1 << OE_SIZE_BITS;
	}
	if (pdata->nr_objects >= pdata->nr_alloc) {
		pdata->nr_alloc = (pdata->nr_alloc  + 1024) * 3 / 2;
		REALLOC_ARRAY(pdata->objects, pdata->nr_alloc);

		if (!pdata->in_pack_by_idx)
			REALLOC_ARRAY(pdata->in_pack, pdata->nr_alloc);
	}

	new_entry = pdata->objects + pdata->nr_objects++;

	memset(new_entry, 0, sizeof(*new_entry));
	hashcpy(new_entry->idx.oid.hash, sha1);

	if (pdata->index_size * 3 <= pdata->nr_objects * 4)
		rehash_objects(pdata);
	else
		pdata->index[index_pos] = pdata->nr_objects;

	if (pdata->in_pack)
		pdata->in_pack[pdata->nr_objects - 1] = NULL;

	return new_entry;
}
