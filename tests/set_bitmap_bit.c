/*
 * set_bitmap_bit.c — LHSR bitmap page manipulator
 *
 * Computes CRC32C using a software table that EXACTLY matches the kernel's
 * __crc32c_le(0, buf, len) function (init=0, no final XOR).
 *
 * Usage: set_bitmap_bit <device> <sector> <bit_number>
 *
 * Reads a 4096-byte bitmap page from <device> at <sector> (512-byte units),
 * sets bit <bit_number> in the bits[] array, recalculates CRC32C over the
 * full page (with crc32=0) using the kernel-matching algorithm, and writes
 * it back.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>

/* ===== Bitmap page structure (matches kernel) ===== */
#define BITMAP_BITS_PER_PAGE 32672
#define BITMAP_HEADER_BYTES  12   /* 8 seq + 4 crc32 */

struct __attribute__((packed)) bitmap_page {
	uint64_t seq;
	uint32_t crc32;
	uint8_t  bits[BITMAP_BITS_PER_PAGE / 8];  /* 4084 bytes */
};

#define PAGE_SIZE 4096

/* ------------------------------------------------------------------ */
/* CRC-32C table (reflected polynomial 0x82F63B78 = 0x1EDC6F41
 * in non-reflected notation).  Same table the kernel's __crc32c_le()
 * uses internally.                                                    */
/* ------------------------------------------------------------------ */
static uint32_t crc32c_table[256];

__attribute__((constructor))
static void build_crc32c_table(void)
{
	uint32_t poly = 0x82F63B78;
	int i, j;
	for (i = 0; i < 256; i++) {
		uint32_t crc = i;
		for (j = 0; j < 8; j++)
			crc = (crc >> 1) ^ (poly & -(crc & 1));
		crc32c_table[i] = crc;
	}
}

/*
 * Kernel-matching CRC32C: init=0, NO final XOR.
 * Matches __crc32c_le(0, buf, len) used by lhsr_bitmap_page_crc().
 * Standard reflected table algorithm: crc = (crc >> 8) ^ table[(crc ^ byte) & 0xFF]
 */
static uint32_t crc32c_kernel(const uint8_t *buf, size_t len)
{
	uint32_t crc = 0;
	size_t i;
	for (i = 0; i < len; i++)
		crc = (crc >> 8) ^ crc32c_table[(crc ^ buf[i]) & 0xFF];
	return crc;
}

/*
 * Compute the CRC of a bitmap page — EXACTLY matches lhsr_bitmap_page_crc().
 * Zeroes the crc32 field before computing.
 */
static uint32_t bitmap_page_crc(struct bitmap_page *page)
{
	uint32_t saved = page->crc32;
	uint32_t csum;

	page->crc32 = 0;
	csum = crc32c_kernel((const uint8_t *)page, PAGE_SIZE);
	page->crc32 = saved;
	return csum;
}

int main(int argc, char **argv)
{
	const char *dev;
	unsigned long sector, bit;
	uint8_t page[PAGE_SIZE];
	struct bitmap_page *bmp = (struct bitmap_page *)page;
	off_t offset;
	int fd, ret = 1;

	if (argc != 4) {
		fprintf(stderr, "Usage: %s <device> <sector> <bit_number>\n", argv[0]);
		return 1;
	}

	dev = argv[1];
	sector = strtoul(argv[2], NULL, 10);
	bit = strtoul(argv[3], NULL, 10);

	if (bit >= BITMAP_BITS_PER_PAGE) {
		fprintf(stderr, "bit %lu out of range (max %u)\n",
			bit, BITMAP_BITS_PER_PAGE - 1);
		return 1;
	}

	offset = (off_t)sector * 512;

	fd = open(dev, O_RDWR);
	if (fd < 0) {
		perror("open");
		return 1;
	}

	if (lseek(fd, offset, SEEK_SET) < 0) {
		perror("lseek");
		goto out;
	}

	if (read(fd, page, PAGE_SIZE) != PAGE_SIZE) {
		perror("read");
		goto out;
	}

	printf("Read bitmap page: seq=%lu crc32=0x%08x bit[0]=0x%02x\n",
	       (unsigned long)bmp->seq, bmp->crc32, bmp->bits[0]);

	/* Verify current CRC */
	uint32_t verify_crc = bitmap_page_crc(bmp);
	printf("CRC verify: stored=0x%08x computed=0x%08x %s\n",
	       bmp->crc32, verify_crc,
	       bmp->crc32 == verify_crc ? "MATCH" : "MISMATCH");

	/* Set the bit */
	unsigned int byte_idx = bit / 8;
	unsigned int bit_offset = bit % 8;
	bmp->bits[byte_idx] |= (1 << bit_offset);

	/* Recompute CRC32C using kernel-matching algorithm */
	bmp->crc32 = bitmap_page_crc(bmp);

	printf("Set bit %lu (byte=%u bit=%u): seq=%lu new_crc32=0x%08x\n",
	       bit, byte_idx, bit_offset,
	       (unsigned long)bmp->seq, bmp->crc32);

	/* Write back */
	if (lseek(fd, offset, SEEK_SET) < 0) {
		perror("lseek write");
		goto out;
	}
	if (write(fd, page, PAGE_SIZE) != PAGE_SIZE) {
		perror("write");
		goto out;
	}

	fsync(fd);
	printf("Wrote %u bytes at sector %lu\n", PAGE_SIZE, sector);
	ret = 0;
out:
	close(fd);
	return ret;
}
