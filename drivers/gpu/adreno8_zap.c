/*
 * gen80000 zap authentication using Qualcomm PAS 13.
 * Format and call ordering: SM8750 vendor mdt_loader.c, qcom_scm.c and kgsl_util.c.
 * The reserved range comes from the annibale zap-shader memory-region.
 */
#include "adreno8_private.h"

extern const uint8_t *hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk_out);
#include "adreno8_zap_private.h"

#define printf adreno8_say

static uint32_t
zap_u32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t
zap_u16(const uint8_t *p)
{
	return (uint16_t)p[0] | (uint16_t)p[1] << 8;
}

static bool
zap_contains(size_t total, uint32_t offset, uint32_t length)
{
	return offset <= total && length <= total - offset;
}

bool
adreno8_zap_parse(const uint8_t *data, size_t size, struct adreno8_zap_image *image)
{
	uint32_t phoff, phnum, maximum = 0;
	bool hash_found = false;

	if (data == NULL || image == NULL || size < 52 ||
	    memcmp(data, "\177ELF\1\1\1", 7) != 0 || zap_u16(data + 40) != 52 ||
	    zap_u16(data + 42) != 32 || zap_u32(data + 20) != 1) {
		return false;
	}
	memset(image, 0, sizeof(*image));
	phoff = zap_u32(data + 28);
	phnum = zap_u16(data + 44);
	if (phoff < 52 || phnum < 2 || phnum > 32 || !zap_contains(size, phoff, phnum * 32)) {
		return false;
	}
	const uint8_t *header = data + phoff;
	image->header_size = zap_u32(header + 16);
	image->minimum = ~0u;
	if (zap_u32(header) == 1 || zap_u32(header + 4) != 0 ||
	    image->header_size < phoff + phnum * 32 || image->header_size > size) {
		return false;
	}
	for (uint32_t i = 1; i < phnum; i++) {
		const uint8_t *ph = data + phoff + i * 32;
		uint32_t type = zap_u32(ph), flags = zap_u32(ph + 24);
		uint32_t offset = zap_u32(ph + 4), address = zap_u32(ph + 12);
		uint32_t filesz = zap_u32(ph + 16), memsz = zap_u32(ph + 20);

		if ((flags & 0x07000000) == 0x02000000) {
			if (hash_found || filesz == 0 || !zap_contains(size, offset, filesz)) {
				return false;
			}
			image->hash_offset = offset;
			image->hash_size = filesz;
			hash_found = true;
			continue;
		}
		if (type != 1 || memsz == 0) {
			continue;
		}
		if (!(flags & 0x08000000) || image->count == ADRENO8_ZAP_SEGMENTS ||
		    filesz > memsz || !zap_contains(size, offset, filesz) ||
		    memsz > ~0u - address || address + memsz > ~0u - 4095u) {
			return false;
		}
		for (unsigned j = 0; j < image->count; j++) {
			const struct adreno8_zap_segment *other = &image->segments[j];
			if ((uint64_t)address < (uint64_t)other->address + other->memory_size &&
			    (uint64_t)other->address < (uint64_t)address + memsz) {
				return false;
			}
		}
		struct adreno8_zap_segment *segment = &image->segments[image->count++];
		segment->file_offset = offset;
		segment->file_size = filesz;
		segment->address = address;
		segment->memory_size = memsz;
		if (address < image->minimum) {
			image->minimum = address;
		}
		if (address + memsz > maximum) {
			maximum = address + memsz;
		}
	}
	if (!hash_found || image->count == 0 || (image->minimum & 4095) ||
	    image->header_size > ADRENO8_ZAP_METADATA_MAX ||
	    image->hash_size > ADRENO8_ZAP_METADATA_MAX - image->header_size) {
		return false;
	}
	image->memory_size = ((maximum + 4095) & ~4095u) - image->minimum;
	return image->memory_size > 0 && image->memory_size <= ADRENO8_ZAP_SIZE;
}

static vm_offset_t zap_metadata;
static bool zap_attempted, zap_authenticated;

bool
adreno8_zap_ready(void)
{
	return zap_authenticated;
}

static bool
zap_pas(uint64_t command, uint64_t arginfo, uint64_t arg1, uint64_t arg2)
{
	uint64_t answer = ~0ULL;
	int32_t status = adreno8_scm_call(command, arginfo, 13, arg1, arg2, 0, &answer);

	if (status != 0 || answer != 0) {
		printf("adreno8: zap: PAS %#llx failed: transport %d, result %#llx\n", command, status, answer);
		return false;
	}
	return true;
}

bool
adreno8_zap_authenticate(void)
{
	struct adreno8_zap_image image;
	uint64_t size = 0, physical;
	vm_offset_t metadata_view, target;
	int disk = 0;
	const uint8_t *data;

	if (zap_authenticated) {
		return true;
	}
	if (zap_attempted || !adreno8_reset_required || PAGE_SIZE < ADRENO8_ZAP_METADATA_MAX) {
		return false;
	}
	data = hakios_image_file("fw", "gen80000_zap.mbn", &size, &disk);
	if (size > 0x100000 || !adreno8_zap_parse(data, (size_t)size, &image)) {
		printf("adreno8: zap: invalid or missing combined gen80000 MBN\n");
		return false;
	}
	/* Retain the page after any PAS call: TZ may still protect it after a failed call. */
	if (kmem_alloc(kernel_map, &zap_metadata, PAGE_SIZE, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	zap_attempted = true;
	physical = kvtophys(zap_metadata);
	if (physical == 0 || (physical & 4095) || kvtophys(zap_metadata + 4095) != physical + 4095) {
		return false;
	}
	adreno8_to_memory(zap_metadata, PAGE_SIZE);
	metadata_view = ml_io_map_wcomb(physical, ADRENO8_ZAP_METADATA_MAX);
	target = ml_io_map_wcomb(ADRENO8_ZAP_BASE, image.memory_size);
	if (metadata_view == 0 || target == 0) {
		return false;
	}
	memcpy((void *)metadata_view, data, image.header_size);
	memcpy((void *)(metadata_view + image.header_size), data + image.hash_offset, image.hash_size);
	adreno8_settle();
	printf("adreno8: zap: metadata %#x bytes, %u segments, reserved %#llx + %#x\n",
	    image.header_size + image.hash_size, image.count, ADRENO8_ZAP_BASE, image.memory_size);
	if (!zap_pas(0x42000201, 0x82, physical, 0) ||
	    !zap_pas(0x42000202, 3, ADRENO8_ZAP_BASE, image.memory_size)) {
		return false;
	}
	for (unsigned i = 0; i < image.count; i++) {
		const struct adreno8_zap_segment *segment = &image.segments[i];
		vm_offset_t at = target + segment->address - image.minimum;
		memcpy((void *)at, data + segment->file_offset, segment->file_size);
		memset((void *)(at + segment->file_size), 0, segment->memory_size - segment->file_size);
	}
	adreno8_settle();
	if (!zap_pas(0x42000205, 1, 0, 0)) {
		return false;
	}
	zap_authenticated = true;
	printf("adreno8: zap: PAS 13 authentication succeeded\n");
	return true;
}
