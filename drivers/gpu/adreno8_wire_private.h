/* GMU firmware records and HFI queue layout used by SM8750. */
#ifndef _DEV_ADRENO8_WIRE_PRIVATE_H
#define _DEV_ADRENO8_WIRE_PRIVATE_H

struct adreno8_fw_block {
	uint32_t address, size, type, value;
};

static inline bool
adreno8_range_contains(uint32_t base, uint32_t bytes, uint32_t address, uint32_t size)
{
	return size != 0 && address >= base && (uint64_t)address + size <= (uint64_t)base + bytes;
}

static inline bool
adreno8_ranges_overlap(uint32_t a, uint32_t a_size, uint32_t b, uint32_t b_size)
{
	return a_size != 0 && b_size != 0 && (uint64_t)a < (uint64_t)b + b_size && (uint64_t)b < (uint64_t)a + a_size;
}

enum adreno8_fw_region { ADRENO8_FW_INVALID, ADRENO8_FW_ITCM, ADRENO8_FW_DTCM, ADRENO8_FW_MEMORY };

/* gen8_gmu_vma: each record must fit entirely within one firmware address region. */
static inline enum adreno8_fw_region
adreno8_fw_region(uint32_t address, uint32_t size)
{
	if (adreno8_range_contains(0, 0x4000, address, size)) {
		return ADRENO8_FW_ITCM;
	}
	if (adreno8_range_contains(0x10004000, 0x4000, address, size)) {
		return ADRENO8_FW_DTCM;
	}
	if (adreno8_range_contains(0x4000, 0xffc000, address, size) ||
	    adreno8_range_contains(0x60000000, 0x20000000, address, size) ||
	    adreno8_range_contains(0xc0000000, 0x20000000, address, size)) {
		return ADRENO8_FW_MEMORY;
	}
	return ADRENO8_FW_INVALID;
}

/* A zero-size record reserves value bytes; size counts bytes present in the file. */
static inline bool
adreno8_fw_next(const uint8_t *data, uint32_t bytes, uint32_t *offset, struct adreno8_fw_block *block)
{
	if (*offset > bytes || bytes - *offset < sizeof(*block)) {
		return false;
	}
	memcpy(block, data + *offset, sizeof(*block));
	if (block->size > bytes - *offset - sizeof(*block) || (block->size & 3) ||
	    (block->size && ((block->address & 3) || adreno8_fw_region(block->address, block->size) == ADRENO8_FW_INVALID))) {
		return false;
	}
	*offset += sizeof(*block) + block->size;
	return true;
}

/* Validate before allocation or TCM writes. Metadata precedes all payload records. */
static inline bool
adreno8_fw_validate(const uint8_t *data, uint32_t bytes)
{
	uint32_t at = 0;
	bool payload = false;
	struct adreno8_fw_block block;

	while (at < bytes) {
		uint32_t record = at, scan = 0;
		bool covered;
		struct adreno8_fw_block prior;

		if (!adreno8_fw_next(data, bytes, &at, &block)) {
			return false;
		}
		if (!block.size) {
			if (payload) {
				return false;
			}
			if ((block.type == 1 || block.type == 7) &&
			    adreno8_fw_region(block.address, block.value) == ADRENO8_FW_INVALID) {
				return false;
			}
			continue;
		}
		payload = true;
		covered = adreno8_fw_region(block.address, block.size) != ADRENO8_FW_MEMORY;
		while (scan < record) {
			if (!adreno8_fw_next(data, bytes, &scan, &prior)) {
				return false;
			}
			if (prior.size && adreno8_ranges_overlap(block.address, block.size, prior.address, prior.size)) {
				return false;
			}
			if (!prior.size && (prior.type == 1 || prior.type == 7) &&
			    adreno8_range_contains(prior.address, prior.value, block.address, block.size)) {
				covered = true;
			}
		}
		if (!covered) {
			return false;
		}
	}
	return payload;
}

#define ADRENO8_HFI_QUEUE_COUNT 3u
#define ADRENO8_HFI_QUEUE_DWORDS 1024u
#define ADRENO8_HFI_MAX_DWORDS 255u
#define ADRENO8_HFI_HEADER(q) (6u + 12u * (q))
#define ADRENO8_HFI_TABLE_DWORDS (6u + 12u * 17u)
#define ADRENO8_HFI_QUEUE_OFFSET(q) \
	(((ADRENO8_HFI_TABLE_DWORDS * 4u + 15u) & ~15u) + (q) * ADRENO8_HFI_QUEUE_DWORDS * 4u)
#define ADRENO8_HFI_ALLOC_BYTES ((ADRENO8_HFI_QUEUE_OFFSET(ADRENO8_HFI_QUEUE_COUNT) + 0xfffu) & ~0xfffu)

/* HWSCHED reserves 17 headers. Ordinary HFI places three queues on 4 KiB boundaries. */
static inline uint32_t
adreno8_hfi_queue_offset(uint32_t queue, bool ordinary)
{
	return ordinary ? (queue + 1u) * 4096u : ADRENO8_HFI_QUEUE_OFFSET(queue);
}

static inline void
adreno8_hfi_init_table(uint32_t *table, uint32_t address, bool ordinary)
{
	memset(table, 0, ADRENO8_HFI_TABLE_DWORDS * 4u);
	/* a6xx_hfi_init stores ordinary table size in bytes; HWSCHED stores dwords. */
	table[1] = ordinary ? (6u + 12u * ADRENO8_HFI_QUEUE_COUNT) * 4u : ADRENO8_HFI_TABLE_DWORDS;
	table[2] = 6;
	table[3] = 12;
	table[4] = table[5] = ADRENO8_HFI_QUEUE_COUNT;
	for (uint32_t q = 0; q < ADRENO8_HFI_QUEUE_COUNT; q++) {
		uint32_t *h = table + ADRENO8_HFI_HEADER(q);
		h[0] = 1;
		h[1] = address + adreno8_hfi_queue_offset(q, ordinary);
		h[2] = ordinary ? 0xa00u | q : q;
		h[3] = ADRENO8_HFI_QUEUE_DWORDS;
		if (ordinary) h[6] = h[7] = h[8] = 1;
	}
}

#endif
