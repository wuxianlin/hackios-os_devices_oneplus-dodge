/* gen80000 combined MBN layout. The parser accepts relocatable ELF32 images only. */
#ifndef _DEV_ADRENO8_ZAP_PRIVATE_H
#define _DEV_ADRENO8_ZAP_PRIVATE_H

#define ADRENO8_ZAP_BASE 0x9b09a000ULL
#define ADRENO8_ZAP_SIZE 0x2000u
#define ADRENO8_ZAP_METADATA_MAX 4096u
#define ADRENO8_ZAP_SEGMENTS 8u

struct adreno8_zap_segment {
	uint32_t file_offset, file_size, address, memory_size;
};

struct adreno8_zap_image {
	uint32_t header_size, hash_offset, hash_size;
	uint32_t minimum, memory_size, count;
	struct adreno8_zap_segment segments[ADRENO8_ZAP_SEGMENTS];
};

bool adreno8_zap_parse(const uint8_t *data, size_t size, struct adreno8_zap_image *image);
bool adreno8_zap_authenticate(void);
bool adreno8_zap_ready(void);

#endif
