#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>

#include "bootimg.h"
#include "magiskboot.h"
#include "utils.h"
#include "logging.h"

#define INSUF_BLOCK_RET    2
#define CHROMEOS_RET       3
#define ELF32_RET          4
#define ELF64_RET          5

// Macros to determine header on-the-go
#define lheader(b, e, o) \
	((b)->flags & PXA_FLAG) ? \
	(((struct pxa_boot_img_hdr*) (b)->hdr)->e o) : \
	(((struct boot_img_hdr*) (b)->hdr)->e o)

#define header(b, e) (lheader(b, e,))

static void dump(void *buf, size_t size, const char *filename) {
	int fd = creat(filename, 0644);
	xwrite(fd, buf, size);
	close(fd);
}

static size_t restore(const char *filename, int fd) {
	int ifd = xopen(filename, O_RDONLY);
	size_t size = lseek(ifd, 0, SEEK_END);
	lseek(ifd, 0, SEEK_SET);
	xsendfile(fd, ifd, NULL, size);
	close(ifd);
	return size;
}

static void restore_buf(int fd, const void *buf, size_t size) {
	xwrite(fd, buf, size);
}

static void print_hdr(const boot_img *boot) {
	fprintf(stderr, "KERNEL [%u]\n", header(boot, kernel_size));
	fprintf(stderr, "RAMDISK [%u]\n", header(boot, ramdisk_size));
	fprintf(stderr, "SECOND [%u]\n", header(boot, second_size));
	fprintf(stderr, "EXTRA [%u]\n", header(boot, extra_size));
	fprintf(stderr, "PAGESIZE [%u]\n", header(boot, page_size));

	if (!(boot->flags & PXA_FLAG)) {
		uint32_t os_version = ((boot_img_hdr*) boot->hdr)->os_version;
		if (os_version) {
			int a,b,c,y,m = 0;
			int version, patch_level;
			version = os_version >> 11;
			patch_level = os_version & 0x7ff;

			a = (version >> 14) & 0x7f;
			b = (version >> 7) & 0x7f;
			c = version & 0x7f;
			fprintf(stderr, "OS_VERSION [%d.%d.%d]\n", a, b, c);

			y = (patch_level >> 4) + 2000;
			m = patch_level & 0xf;
			fprintf(stderr, "PATCH_LEVEL [%d-%02d]\n", y, m);
		}
	}

	fprintf(stderr, "NAME [%s]\n", header(boot, name));
	fprintf(stderr, "CMDLINE [%s]\n", header(boot, cmdline));
}

static void clean_boot(boot_img *boot) {
	munmap(boot->map_addr, boot->map_size);
	free(boot->hdr);
	free(boot->k_hdr);
	free(boot->r_hdr);
	memset(boot, 0, sizeof(*boot));
}

int parse_img(const char *image, boot_img *boot) {
	memset(boot, 0, sizeof(*boot));
	int is_blk = mmap_ro(image, &boot->map_addr, &boot->map_size);

	// Parse image
	fprintf(stderr, "Parsing boot image: [%s]\n", image);
	for (void *head = boot->map_addr; head < boot->map_addr + boot->map_size; head += 256) {
		size_t pos = 0;

		switch (check_fmt(head)) {
		case CHROMEOS:
			// The caller should know it's chromeos, as it needs additional signing
			boot->flags |= CHROMEOS_FLAG;
			continue;
		case ELF32:
			exit(ELF32_RET);
		case ELF64:
			exit(ELF64_RET);
		case AOSP:
			// Read the header
			if (((boot_img_hdr*) head)->page_size >= 0x02000000) {
				boot->flags |= PXA_FLAG;
				fprintf(stderr, "PXA_BOOT_HDR\n");
				boot->hdr = malloc(sizeof(pxa_boot_img_hdr));
				memcpy(boot->hdr, head, sizeof(pxa_boot_img_hdr));
			} else {
				boot->hdr = malloc(sizeof(boot_img_hdr));
				memcpy(boot->hdr, head, sizeof(boot_img_hdr));
			}
			pos += header(boot, page_size);

			print_hdr(boot);

			boot->kernel = head + pos;
			pos += header(boot, kernel_size);
			mem_align(&pos, header(boot, page_size));

			boot->ramdisk = head + pos;
			pos += header(boot, ramdisk_size);
			mem_align(&pos, header(boot, page_size));

			if (header(boot, second_size)) {
				boot->second = head + pos;
				pos += header(boot, second_size);
				mem_align(&pos, header(boot, page_size));
			}

			if (header(boot, extra_size)) {
				boot->extra = head + pos;
				pos += header(boot, extra_size);
				mem_align(&pos, header(boot, page_size));
			}

			if (pos < boot->map_size) {
				boot->tail = head + pos;
				boot->tail_size = boot->map_size - pos;
			}

			// Search for dtb in kernel
			for (uint32_t i = 0; i < header(boot, kernel_size); ++i) {
				if (memcmp(boot->kernel + i, DTB_MAGIC, 4) == 0) {
					boot->dtb = boot->kernel + i;
					boot->dt_size = header(boot, kernel_size) - i;
					lheader(boot, kernel_size, = i);
					fprintf(stderr, "DTB [%u]\n", boot->dt_size);
					break;
				}
			}

			boot->k_fmt = check_fmt(boot->kernel);
			boot->r_fmt = check_fmt(boot->ramdisk);

			// Check MTK
			if (boot->k_fmt == MTK) {
				fprintf(stderr, "MTK_KERNEL_HDR\n");
				boot->flags |= MTK_KERNEL;
				boot->k_hdr = malloc(sizeof(mtk_hdr));
				memcpy(boot->k_hdr, boot->kernel, sizeof(mtk_hdr));
				fprintf(stderr, "KERNEL [%u]\n", boot->k_hdr->size);
				fprintf(stderr, "NAME [%s]\n", boot->k_hdr->name);
				boot->kernel += 512;
				lheader(boot, kernel_size, -= 512);
				boot->k_fmt = check_fmt(boot->kernel);
			}
			if (boot->r_fmt == MTK) {
				fprintf(stderr, "MTK_RAMDISK_HDR\n");
				boot->flags |= MTK_RAMDISK;
				boot->r_hdr = malloc(sizeof(mtk_hdr));
				memcpy(boot->r_hdr, boot->kernel, sizeof(mtk_hdr));
				fprintf(stderr, "RAMDISK [%u]\n", boot->r_hdr->size);
				fprintf(stderr, "NAME [%s]\n", boot->r_hdr->name);
				boot->ramdisk += 512;
				lheader(boot, ramdisk_size, -= 512);
				boot->r_fmt = check_fmt(boot->ramdisk);
			}

			char fmt[16];
			get_fmt_name(boot->k_fmt, fmt);
			fprintf(stderr, "KERNEL_FMT [%s]\n", fmt);
			get_fmt_name(boot->r_fmt, fmt);
			fprintf(stderr, "RAMDISK_FMT [%s]\n", fmt);

			return boot->flags & CHROMEOS_FLAG ? CHROMEOS_RET :
				   ((is_blk && boot->tail_size < 500 * 1024) ? INSUF_BLOCK_RET : 0);
		default:
			continue;
		}
	}
	LOGE("No boot image magic found!\n");
}

int unpack(const char *image) {
	boot_img boot;
	int ret = parse_img(image, &boot);
	int fd;

	// Dump kernel
	if (COMPRESSED(boot.k_fmt)) {
		fd = creat(KERNEL_FILE, 0644);
		decomp(boot.k_fmt, fd, boot.kernel, header(&boot, kernel_size));
		close(fd);
	} else {
		dump(boot.kernel, header(&boot, kernel_size), KERNEL_FILE);
	}

	if (boot.dt_size) {
		// Dump dtb
		dump(boot.dtb, boot.dt_size, DTB_FILE);
	}

	// Dump ramdisk
	if (COMPRESSED(boot.r_fmt)) {
		fd = creat(RAMDISK_FILE, 0644);
		decomp(boot.r_fmt, fd, boot.ramdisk, header(&boot, ramdisk_size));
		close(fd);
	} else {
		dump(boot.ramdisk, header(&boot, ramdisk_size), RAMDISK_FILE ".raw");
		LOGE("Unknown ramdisk format! Dumped to %s\n", RAMDISK_FILE ".raw");
	}

	if (header(&boot, second_size)) {
		// Dump second
		dump(boot.second, header(&boot, second_size), SECOND_FILE);
	}

	if (header(&boot, extra_size)) {
		// Dump extra
		dump(boot.extra, header(&boot, extra_size), EXTRA_FILE);
	}

	clean_boot(&boot);
	return ret;
}

void repack(const char* orig_image, const char* out_image) {
	boot_img boot;

	// There are possible two MTK headers
	off_t mtk_kernel_off, mtk_ramdisk_off;

	// Parse original image
	parse_img(orig_image, &boot);

	fprintf(stderr, "Repack to boot image: [%s]\n", out_image);

	// Create new image
	int fd = creat(out_image, 0644);

	// Skip a page for header
	write_zero(fd, header(&boot, page_size));

	if (boot.flags & MTK_KERNEL) {
		// Record position and skip MTK header
		mtk_kernel_off = lseek(fd, 0, SEEK_CUR);
		write_zero(fd, 512);
	}
	if (COMPRESSED(boot.k_fmt)) {
		size_t raw_size;
		void *kernel_raw;
		mmap_ro(KERNEL_FILE, &kernel_raw, &raw_size);
		lheader(&boot, kernel_size, = comp(boot.k_fmt, fd, kernel_raw, raw_size));
		munmap(kernel_raw, raw_size);
	} else {
		lheader(&boot, kernel_size, = restore(KERNEL_FILE, fd));
	}
	// Restore dtb
	if (boot.dt_size && access(DTB_FILE, R_OK) == 0) {
		lheader(&boot, kernel_size, += restore(DTB_FILE, fd));
	}
	file_align(fd, header(&boot, page_size), 1);

	if (boot.flags & MTK_RAMDISK) {
		// Record position and skip MTK header
		mtk_ramdisk_off = lseek(fd, 0, SEEK_CUR);
		write_zero(fd, 512);
	}
	if (access(RAMDISK_FILE, R_OK) == 0) {
		// If we found raw cpio, compress to original format
		size_t cpio_size;
		void *cpio;
		mmap_ro(RAMDISK_FILE, &cpio, &cpio_size);
		lheader(&boot, ramdisk_size, = comp(boot.r_fmt, fd, cpio, cpio_size));
		munmap(cpio, cpio_size);
	} else {
		// Find compressed ramdisk
		char name[PATH_MAX];
		int found = 0;
		for (int i = 0; SUP_EXT_LIST[i]; ++i) {
			sprintf(name, "%s.%s", RAMDISK_FILE, SUP_EXT_LIST[i]);
			if (access(name, R_OK) == 0) {
				found = 1;
				break;
			}
		}
		if (!found)
			LOGE("No ramdisk exists!\n");
		lheader(&boot, ramdisk_size, = restore(name, fd));
	}
	file_align(fd, header(&boot, page_size), 1);

	// Restore second
	if (header(&boot, second_size) && access(SECOND_FILE, R_OK) == 0) {
		lheader(&boot, second_size, = restore(SECOND_FILE, fd));
		file_align(fd, header(&boot, page_size), 1);
	}

	// Restore extra
	if (header(&boot, extra_size) && access(EXTRA_FILE, R_OK) == 0) {
		lheader(&boot, extra_size, = restore(EXTRA_FILE, fd));
		file_align(fd, header(&boot, page_size), 1);
	}

	// Check tail info, currently only for LG Bump and Samsung SEANDROIDENFORCE
	if (boot.tail_size >= 16) {
		if (memcmp(boot.tail, "SEANDROIDENFORCE", 16) == 0 ||
			memcmp(boot.tail, LG_BUMP_MAGIC, 16) == 0 ) {
			restore_buf(fd, boot.tail, 16);
		}
	}

	// Write MTK headers back
	if (boot.flags & MTK_KERNEL) {
		lseek(fd, mtk_kernel_off, SEEK_SET);
		boot.k_hdr->size = header(&boot, kernel_size);
		lheader(&boot, kernel_size, += 512);
		restore_buf(fd, boot.k_hdr, sizeof(mtk_hdr));
	}
	if (boot.flags & MTK_RAMDISK) {
		lseek(fd, mtk_ramdisk_off, SEEK_SET);
		boot.r_hdr->size = header(&boot, ramdisk_size);
		lheader(&boot, ramdisk_size, += 512);
		restore_buf(fd, boot.r_hdr, sizeof(mtk_hdr));
	}
	// Main header
	lseek(fd, 0, SEEK_SET);
	restore_buf(fd, boot.hdr, (boot.flags & PXA_FLAG) ? sizeof(pxa_boot_img_hdr) : sizeof(boot_img_hdr));

	// Print new image info
	print_hdr(&boot);

	clean_boot(&boot);
	close(fd);
}