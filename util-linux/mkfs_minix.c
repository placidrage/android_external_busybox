/* vi: set sw=4 ts=4: */
/*
 * mkfs.c - make a linux (minix) file-system.
 *
 * (C) 1991 Linus Torvalds. This file may be redistributed as per
 * the Linux copyright.
 */

/*
 * DD.MM.YY
 *
 * 24.11.91  -	Time began. Used the fsck sources to get started.
 *
 * 25.11.91  -	Corrected some bugs. Added support for ".badblocks"
 *		The algorithm for ".badblocks" is a bit weird, but
 *		it should work. Oh, well.
 *
 * 25.01.92  -	Added the -l option for getting the list of bad blocks
 *		out of a named file. (Dave Rivers, rivers@ponds.uucp)
 *
 * 28.02.92  -	Added %-information when using -c.
 *
 * 28.02.93  -	Added support for other namelengths than the original
 *		14 characters so that I can test the new kernel routines..
 *
 * 09.10.93  -	Make exit status conform to that required by fsutil
 *		(Rik Faith, faith@cs.unc.edu)
 *
 * 31.10.93  -	Added inode request feature, for backup floppies: use
 *		32 inodes, for a news partition use more.
 *		(Scott Heavner, sdh@po.cwru.edu)
 *
 * 03.01.94  -	Added support for file system valid flag.
 *		(Dr. Wettstein, greg%wind.uucp@plains.nodak.edu)
 *
 * 30.10.94 - added support for v2 filesystem
 *	      (Andreas Schwab, schwab@issan.informatik.uni-dortmund.de)
 *
 * 09.11.94  -	Added test to prevent overwrite of mounted fs adapted
 *		from Theodore Ts'o's (tytso@athena.mit.edu) mke2fs
 *		program.  (Daniel Quinlan, quinlan@yggdrasil.com)
 *
 * 03.20.95  -	Clear first 512 bytes of filesystem to make certain that
 *		the filesystem is not misidentified as a MS-DOS FAT filesystem.
 *		(Daniel Quinlan, quinlan@yggdrasil.com)
 *
 * 02.07.96  -  Added small patch from Russell King to make the program a
 *		good deal more portable (janl@math.uio.no)
 *
 * Usage:  mkfs [-c | -l filename ] [-v] [-nXX] [-iXX] device [size-in-blocks]
 *
 *	-c for readability checking (SLOW!)
 *      -l for getting a list of bad blocks from a file.
 *	-n for namelength (currently the kernel only uses 14 or 30)
 *	-i for number of inodes
 *	-v for v2 filesystem
 *
 * The device may be a block device or a image of one, but this isn't
 * enforced (but it's not much fun on a character device :-).
 *
 * Modified for BusyBox by Erik Andersen <andersen@debian.org> --
 *	removed getopt based parser and added a hand rolled one.
 */

#include "busybox.h"
#include <mntent.h>

#define MINIX_ROOT_INO 1
#define MINIX_LINK_MAX	250
#define MINIX2_LINK_MAX	65530

#define MINIX_I_MAP_SLOTS	8
#define MINIX_Z_MAP_SLOTS	64
#define MINIX_SUPER_MAGIC	0x137F		/* original minix fs */
#define MINIX_SUPER_MAGIC2	0x138F		/* minix fs, 30 char names */
#define MINIX2_SUPER_MAGIC	0x2468		/* minix V2 fs */
#define MINIX2_SUPER_MAGIC2	0x2478		/* minix V2 fs, 30 char names */
#define MINIX_VALID_FS		0x0001		/* Clean fs. */
#define MINIX_ERROR_FS		0x0002		/* fs has errors. */

#define MINIX_INODES_PER_BLOCK ((BLOCK_SIZE)/(sizeof (struct minix_inode)))
#define MINIX2_INODES_PER_BLOCK ((BLOCK_SIZE)/(sizeof (struct minix2_inode)))

#define MINIX_V1		0x0001		/* original minix fs */
#define MINIX_V2		0x0002		/* minix V2 fs */

#define INODE_VERSION(inode)	inode->i_sb->u.minix_sb.s_version

/*
 * This is the original minix inode layout on disk.
 * Note the 8-bit gid and atime and ctime.
 */
struct minix_inode {
	uint16_t i_mode;
	uint16_t i_uid;
	uint32_t i_size;
	uint32_t i_time;
	uint8_t  i_gid;
	uint8_t  i_nlinks;
	uint16_t i_zone[9];
};

/*
 * The new minix inode has all the time entries, as well as
 * long block numbers and a third indirect block (7+1+1+1
 * instead of 7+1+1). Also, some previously 8-bit values are
 * now 16-bit. The inode is now 64 bytes instead of 32.
 */
struct minix2_inode {
	uint16_t i_mode;
	uint16_t i_nlinks;
	uint16_t i_uid;
	uint16_t i_gid;
	uint32_t i_size;
	uint32_t i_atime;
	uint32_t i_mtime;
	uint32_t i_ctime;
	uint32_t i_zone[10];
};

/*
 * minix super-block data on disk
 */
struct minix_super_block {
	uint16_t s_ninodes;
	uint16_t s_nzones;
	uint16_t s_imap_blocks;
	uint16_t s_zmap_blocks;
	uint16_t s_firstdatazone;
	uint16_t s_log_zone_size;
	uint32_t s_max_size;
	uint16_t s_magic;
	uint16_t s_state;
	uint32_t s_zones;
};

struct minix_dir_entry {
	uint16_t inode;
	char name[0];
};

#define NAME_MAX         255   /* # chars in a file name */

#define MINIX_INODES_PER_BLOCK ((BLOCK_SIZE)/(sizeof (struct minix_inode)))

#define MINIX_VALID_FS               0x0001          /* Clean fs. */
#define MINIX_ERROR_FS               0x0002          /* fs has errors. */

#define MINIX_SUPER_MAGIC    0x137F          /* original minix fs */
#define MINIX_SUPER_MAGIC2   0x138F          /* minix fs, 30 char names */

#ifndef BLKGETSIZE
#define BLKGETSIZE _IO(0x12,96)    /* return device size */
#endif


#ifndef __linux__
#define volatile
#endif

#define MINIX_ROOT_INO 1
#define MINIX_BAD_INO 2

#define TEST_BUFFER_BLOCKS 16
#define MAX_GOOD_BLOCKS 512

#define UPPER(size,n) (((size)+((n)-1))/(n))
#define INODE_SIZE (sizeof(struct minix_inode))

#if ENABLE_FEATURE_MINIX2
# define INODE_SIZE2 (sizeof(struct minix2_inode))
# define INODE_BLOCKS UPPER(INODES, (version2 ? MINIX2_INODES_PER_BLOCK \
				    : MINIX_INODES_PER_BLOCK))
#else
# define INODE_BLOCKS UPPER(INODES, (MINIX_INODES_PER_BLOCK))
#endif

#define INODE_BUFFER_SIZE (INODE_BLOCKS * BLOCK_SIZE)

#define BITS_PER_BLOCK (BLOCK_SIZE<<3)

static char *device_name;
static int DEV = -1;
static uint32_t BLOCKS;
static int badblocks;
/* default (changed to 30, per Linus's suggestion, Sun Nov 21 08:05:07 1993) */
static int namelen = 30;
static int dirsize = 32;
static int magic = MINIX_SUPER_MAGIC2;
#if ENABLE_FEATURE_MINIX2
static int version2;
#else
enum { version2 = 0 };
#endif

static char root_block[BLOCK_SIZE];

static char *inode_buffer;

#define Inode (((struct minix_inode *) inode_buffer)-1)
#if ENABLE_FEATURE_MINIX2
#define Inode2 (((struct minix2_inode *) inode_buffer)-1)
#endif
static char super_block_buffer[BLOCK_SIZE];
static char boot_block_buffer[512];

#define Super (*(struct minix_super_block *)super_block_buffer)

#define INODES (Super.s_ninodes)
#if ENABLE_FEATURE_MINIX2
# define ZONES (version2 ? Super.s_zones : Super.s_nzones)
#else
# define ZONES (Super.s_nzones)
#endif

#define IMAPS (Super.s_imap_blocks)
#define ZMAPS (Super.s_zmap_blocks)
#define FIRSTZONE (Super.s_firstdatazone)
#define ZONESIZE (Super.s_log_zone_size)
#define MAXSIZE (Super.s_max_size)
#define MAGIC (Super.s_magic)
#define NORM_FIRSTZONE (2+IMAPS+ZMAPS+INODE_BLOCKS)

static char *inode_map;
static char *zone_map;

static unsigned short good_blocks_table[MAX_GOOD_BLOCKS];
static int used_good_blocks;
static unsigned long req_nr_inodes;

static int bit(char* a, unsigned i)
{
	  return (a[i >> 3] & (1<<(i & 7))) != 0;
}
#define inode_in_use(x) (bit(inode_map,(x)))
#define zone_in_use(x) (bit(zone_map,(x)-FIRSTZONE+1))

#define mark_inode(x) (setbit(inode_map,(x)))
#define unmark_inode(x) (clrbit(inode_map,(x)))

#define mark_zone(x) (setbit(zone_map,(x)-FIRSTZONE+1))
#define unmark_zone(x) (clrbit(zone_map,(x)-FIRSTZONE+1))

static long valid_offset(int fd, int offset)
{
	char ch;

	if (lseek(fd, offset, SEEK_SET) < 0)
		return 0;
	if (read(fd, &ch, 1) < 1)
		return 0;
	return 1;
}

static int count_blocks(int fd)
{
	int high, low;

	low = 0;
	for (high = 1; valid_offset(fd, high); high *= 2)
		low = high;

	while (low < high - 1) {
		const int mid = (low + high) / 2;

		if (valid_offset(fd, mid))
			low = mid;
		else
			high = mid;
	}
	valid_offset(fd, 0);
	return (low + 1);
}

static int get_size(const char *file)
{
	int fd;
	long size;

	fd = xopen(file, O_RDWR);
	if (ioctl(fd, BLKGETSIZE, &size) >= 0) {
		close(fd);
		return (size * 512);
	}

	size = count_blocks(fd);
	close(fd);
	return size;
}

static void write_tables(void)
{
	/* Mark the super block valid. */
	Super.s_state |= MINIX_VALID_FS;
	Super.s_state &= ~MINIX_ERROR_FS;

	msg_eol = "seek to 0 failed";
	xlseek(DEV, 0, SEEK_SET);

	msg_eol = "cannot clear boot sector";
	xwrite(DEV, boot_block_buffer, 512);

	msg_eol = "seek to BLOCK_SIZE failed";
	xlseek(DEV, BLOCK_SIZE, SEEK_SET);

	msg_eol = "cannot write superblock";
	xwrite(DEV, super_block_buffer, BLOCK_SIZE);

	msg_eol = "cannot write inode map";
	xwrite(DEV, inode_map, IMAPS * BLOCK_SIZE);

	msg_eol = "cannot write zone map";
	xwrite(DEV, zone_map, ZMAPS * BLOCK_SIZE);

	msg_eol = "cannot write inodes";
	xwrite(DEV, inode_buffer, INODE_BUFFER_SIZE);

	msg_eol = "\n";
}

static void write_block(int blk, char *buffer)
{
	xlseek(DEV, blk * BLOCK_SIZE, SEEK_SET);
	xwrite(DEV, buffer, BLOCK_SIZE);
}

static int get_free_block(void)
{
	int blk;

	if (used_good_blocks + 1 >= MAX_GOOD_BLOCKS)
		bb_error_msg_and_die("too many bad blocks");
	if (used_good_blocks)
		blk = good_blocks_table[used_good_blocks - 1] + 1;
	else
		blk = FIRSTZONE;
	while (blk < ZONES && zone_in_use(blk))
		blk++;
	if (blk >= ZONES)
		bb_error_msg_and_die("not enough good blocks");
	good_blocks_table[used_good_blocks] = blk;
	used_good_blocks++;
	return blk;
}

static void mark_good_blocks(void)
{
	int blk;

	for (blk = 0; blk < used_good_blocks; blk++)
		mark_zone(good_blocks_table[blk]);
}

static int next(int zone)
{
	if (!zone)
		zone = FIRSTZONE - 1;
	while (++zone < ZONES)
		if (zone_in_use(zone))
			return zone;
	return 0;
}

static void make_bad_inode(void)
{
	struct minix_inode *inode = &Inode[MINIX_BAD_INO];
	int i, j, zone;
	int ind = 0, dind = 0;
	unsigned short ind_block[BLOCK_SIZE >> 1];
	unsigned short dind_block[BLOCK_SIZE >> 1];

#define NEXT_BAD (zone = next(zone))

	if (!badblocks)
		return;
	mark_inode(MINIX_BAD_INO);
	inode->i_nlinks = 1;
	/* BTW, setting this makes all images different */
	/* it's harder to check for bugs then - diff isn't helpful :(... */
	inode->i_time = time(NULL);
	inode->i_mode = S_IFREG + 0000;
	inode->i_size = badblocks * BLOCK_SIZE;
	zone = next(0);
	for (i = 0; i < 7; i++) {
		inode->i_zone[i] = zone;
		if (!NEXT_BAD)
			goto end_bad;
	}
	inode->i_zone[7] = ind = get_free_block();
	memset(ind_block, 0, BLOCK_SIZE);
	for (i = 0; i < 512; i++) {
		ind_block[i] = zone;
		if (!NEXT_BAD)
			goto end_bad;
	}
	inode->i_zone[8] = dind = get_free_block();
	memset(dind_block, 0, BLOCK_SIZE);
	for (i = 0; i < 512; i++) {
		write_block(ind, (char *) ind_block);
		dind_block[i] = ind = get_free_block();
		memset(ind_block, 0, BLOCK_SIZE);
		for (j = 0; j < 512; j++) {
			ind_block[j] = zone;
			if (!NEXT_BAD)
				goto end_bad;
		}
	}
	bb_error_msg_and_die("too many bad blocks");
  end_bad:
	if (ind)
		write_block(ind, (char *) ind_block);
	if (dind)
		write_block(dind, (char *) dind_block);
}

#if ENABLE_FEATURE_MINIX2
static void make_bad_inode2(void)
{
	struct minix2_inode *inode = &Inode2[MINIX_BAD_INO];
	int i, j, zone;
	int ind = 0, dind = 0;
	unsigned long ind_block[BLOCK_SIZE >> 2];
	unsigned long dind_block[BLOCK_SIZE >> 2];

	if (!badblocks)
		return;
	mark_inode(MINIX_BAD_INO);
	inode->i_nlinks = 1;
	inode->i_atime = inode->i_mtime = inode->i_ctime = time(NULL);
	inode->i_mode = S_IFREG + 0000;
	inode->i_size = badblocks * BLOCK_SIZE;
	zone = next(0);
	for (i = 0; i < 7; i++) {
		inode->i_zone[i] = zone;
		if (!NEXT_BAD)
			goto end_bad;
	}
	inode->i_zone[7] = ind = get_free_block();
	memset(ind_block, 0, BLOCK_SIZE);
	for (i = 0; i < 256; i++) {
		ind_block[i] = zone;
		if (!NEXT_BAD)
			goto end_bad;
	}
	inode->i_zone[8] = dind = get_free_block();
	memset(dind_block, 0, BLOCK_SIZE);
	for (i = 0; i < 256; i++) {
		write_block(ind, (char *) ind_block);
		dind_block[i] = ind = get_free_block();
		memset(ind_block, 0, BLOCK_SIZE);
		for (j = 0; j < 256; j++) {
			ind_block[j] = zone;
			if (!NEXT_BAD)
				goto end_bad;
		}
	}
	/* Could make triple indirect block here */
	bb_error_msg_and_die("too many bad blocks");
  end_bad:
	if (ind)
		write_block(ind, (char *) ind_block);
	if (dind)
		write_block(dind, (char *) dind_block);
}
#endif

static void make_root_inode(void)
{
	struct minix_inode *inode = &Inode[MINIX_ROOT_INO];

	mark_inode(MINIX_ROOT_INO);
	inode->i_zone[0] = get_free_block();
	inode->i_nlinks = 2;
	inode->i_time = time(NULL);
	if (badblocks)
		inode->i_size = 3 * dirsize;
	else {
		root_block[2 * dirsize] = '\0';
		root_block[2 * dirsize + 1] = '\0';
		inode->i_size = 2 * dirsize;
	}
	inode->i_mode = S_IFDIR + 0755;
	inode->i_uid = getuid();
	if (inode->i_uid)
		inode->i_gid = getgid();
	write_block(inode->i_zone[0], root_block);
}

#if ENABLE_FEATURE_MINIX2
static void make_root_inode2(void)
{
	struct minix2_inode *inode = &Inode2[MINIX_ROOT_INO];

	mark_inode(MINIX_ROOT_INO);
	inode->i_zone[0] = get_free_block();
	inode->i_nlinks = 2;
	inode->i_atime = inode->i_mtime = inode->i_ctime = time(NULL);
	if (badblocks)
		inode->i_size = 3 * dirsize;
	else {
		root_block[2 * dirsize] = '\0';
		root_block[2 * dirsize + 1] = '\0';
		inode->i_size = 2 * dirsize;
	}
	inode->i_mode = S_IFDIR + 0755;
	inode->i_uid = getuid();
	if (inode->i_uid)
		inode->i_gid = getgid();
	write_block(inode->i_zone[0], root_block);
}
#endif

static void setup_tables(void)
{
	int i;
	unsigned long inodes;

	memset(super_block_buffer, 0, BLOCK_SIZE);
	memset(boot_block_buffer, 0, 512);
	MAGIC = magic;
	ZONESIZE = 0;
	MAXSIZE = version2 ? 0x7fffffff : (7 + 512 + 512 * 512) * 1024;
	if (version2) {
		Super.s_zones =  BLOCKS;
	} else
		Super.s_nzones = BLOCKS;

/* some magic nrs: 1 inode / 3 blocks */
	if (req_nr_inodes == 0)
		inodes = BLOCKS / 3;
	else
		inodes = req_nr_inodes;
	/* Round up inode count to fill block size */
	if (version2)
		inodes = ((inodes + MINIX2_INODES_PER_BLOCK - 1) &
				  ~(MINIX2_INODES_PER_BLOCK - 1));
	else
		inodes = ((inodes + MINIX_INODES_PER_BLOCK - 1) &
				  ~(MINIX_INODES_PER_BLOCK - 1));
	if (inodes > 65535)
		inodes = 65535;
	INODES = inodes;
	IMAPS = UPPER(INODES + 1, BITS_PER_BLOCK);
	ZMAPS = 0;
	i = 0;
	while (ZMAPS !=
		   UPPER(BLOCKS - (2 + IMAPS + ZMAPS + INODE_BLOCKS) + 1,
				 BITS_PER_BLOCK) && i < 1000) {
		ZMAPS =
			UPPER(BLOCKS - (2 + IMAPS + ZMAPS + INODE_BLOCKS) + 1,
				  BITS_PER_BLOCK);
		i++;
	}
	/* Real bad hack but overwise mkfs.minix can be thrown
	 * in infinite loop...
	 * try:
	 * dd if=/dev/zero of=test.fs count=10 bs=1024
	 * /sbin/mkfs.minix -i 200 test.fs
	 * */
	if (i >= 999) {
		bb_error_msg_and_die("cannot allocate buffers for maps");
	}
	FIRSTZONE = NORM_FIRSTZONE;
	inode_map = xmalloc(IMAPS * BLOCK_SIZE);
	zone_map = xmalloc(ZMAPS * BLOCK_SIZE);
	memset(inode_map, 0xff, IMAPS * BLOCK_SIZE);
	memset(zone_map, 0xff, ZMAPS * BLOCK_SIZE);
	for (i = FIRSTZONE; i < ZONES; i++)
		unmark_zone(i);
	for (i = MINIX_ROOT_INO; i <= INODES; i++)
		unmark_inode(i);
	inode_buffer = xmalloc(INODE_BUFFER_SIZE);
	memset(inode_buffer, 0, INODE_BUFFER_SIZE);
	printf("%ld inodes\n", (long)INODES);
	printf("%ld blocks\n", (long)ZONES);
	printf("Firstdatazone=%ld (%ld)\n", (long)FIRSTZONE, (long)NORM_FIRSTZONE);
	printf("Zonesize=%d\n", BLOCK_SIZE << ZONESIZE);
	printf("Maxsize=%ld\n\n", (long)MAXSIZE);
}

/*
 * Perform a test of a block; return the number of
 * blocks readable/writable.
 */
static long do_check(char *buffer, int try, unsigned current_block)
{
	long got;

	/* Seek to the correct loc. */
	msg_eol = "seek failed during testing of blocks";
	xlseek(DEV, current_block * BLOCK_SIZE, SEEK_SET);
	msg_eol = "\n";

	/* Try the read */
	got = read(DEV, buffer, try * BLOCK_SIZE);
	if (got < 0)
		got = 0;
	if (got & (BLOCK_SIZE - 1)) {
		printf("Weird values in do_check: probably bugs\n");
	}
	got /= BLOCK_SIZE;
	return got;
}

static unsigned currently_testing;

static void alarm_intr(int alnum)
{
	if (currently_testing >= ZONES)
		return;
	signal(SIGALRM, alarm_intr);
	alarm(5);
	if (!currently_testing)
		return;
	printf("%d ...", currently_testing);
	fflush(stdout);
}

static void check_blocks(void)
{
	int try, got;
	static char buffer[BLOCK_SIZE * TEST_BUFFER_BLOCKS];

	currently_testing = 0;
	signal(SIGALRM, alarm_intr);
	alarm(5);
	while (currently_testing < ZONES) {
		msg_eol = "seek failed in check_blocks";
		xlseek(DEV, currently_testing * BLOCK_SIZE, SEEK_SET);
		msg_eol = "\n";
		try = TEST_BUFFER_BLOCKS;
		if (currently_testing + try > ZONES)
			try = ZONES - currently_testing;
		got = do_check(buffer, try, currently_testing);
		currently_testing += got;
		if (got == try)
			continue;
		if (currently_testing < FIRSTZONE)
			bb_error_msg_and_die("bad blocks before data-area: cannot make fs");
		mark_zone(currently_testing);
		badblocks++;
		currently_testing++;
	}
	printf("%d bad block(s)\n", badblocks);
}

static void get_list_blocks(char *filename)
{
	FILE *listfile;
	unsigned long blockno;

	listfile = xfopen(filename, "r");
	while (!feof(listfile)) {
		fscanf(listfile, "%ld\n", &blockno);
		mark_zone(blockno);
		badblocks++;
	}
	printf("%d bad block(s)\n", badblocks);
}

int mkfs_minix_main(int argc, char **argv)
{
	unsigned opt;
	char *tmp;
	struct stat statbuf;
	char *str_i, *str_n;
	char *listfile = NULL;

	if (INODE_SIZE * MINIX_INODES_PER_BLOCK != BLOCK_SIZE)
		bb_error_msg_and_die("bad inode size");
#if ENABLE_FEATURE_MINIX2
	if (INODE_SIZE2 * MINIX2_INODES_PER_BLOCK != BLOCK_SIZE)
		bb_error_msg_and_die("bad inode size");
#endif

	opt = getopt32(argc, argv, "ci:l:n:v", &str_i, &listfile, &str_n);
	argv += optind;
	//if (opt & 1) -c
	if (opt & 2) req_nr_inodes = xatoul(str_i); // -i
	//if (opt & 4) -l
	if (opt & 8) { // -n
		namelen = xatoi_u(str_n);
		if (namelen == 14) magic = MINIX_SUPER_MAGIC;
		else if (namelen == 30) magic = MINIX_SUPER_MAGIC2;
		else bb_show_usage();
		dirsize = namelen + 2;
	}
	if (opt & 0x10) { // -v
#if ENABLE_FEATURE_MINIX2
		version2 = 1;
#else
		bb_error_msg_and_die("%s: not compiled with minix v2 support",
			device_name);
#endif
	}

	device_name = *argv++;
	if (!device_name)
		bb_show_usage();
	if (*argv) {
		BLOCKS = xatou32(*argv);
		if (BLOCKS < 10)
			bb_show_usage();
	} else
		BLOCKS = get_size(device_name) / 1024;

	if (version2) {
		magic = MINIX2_SUPER_MAGIC2;
		if (namelen == 14)
			magic = MINIX2_SUPER_MAGIC;
	} else if (BLOCKS > 65535)
		BLOCKS = 65535;

	if (find_mount_point(device_name, NULL))
		bb_error_msg_and_die("%s is mounted; refusing to make "
			"a filesystem", device_name);

	tmp = root_block;
	*(short *) tmp = 1;
	strcpy(tmp + 2, ".");
	tmp += dirsize;
	*(short *) tmp = 1;
	strcpy(tmp + 2, "..");
	tmp += dirsize;
	*(short *) tmp = 2;
	strcpy(tmp + 2, ".badblocks");

	DEV = xopen(device_name, O_RDWR);
	if (fstat(DEV, &statbuf) < 0)
		bb_error_msg_and_die("cannot stat %s", device_name);
	if (!S_ISBLK(statbuf.st_mode))
		opt &= ~1; // clear -c (check)
	else if (statbuf.st_rdev == 0x0300 || statbuf.st_rdev == 0x0340)
		bb_error_msg_and_die("will not try to make filesystem on '%s'", device_name);

	setup_tables();

	if (opt & 1) // -c ?
		check_blocks();
	else if (listfile)
		get_list_blocks(listfile);

	if (version2) {
		make_root_inode2();
		make_bad_inode2();
	} else {
		make_root_inode();
		make_bad_inode();
	}

	mark_good_blocks();
	write_tables();
	return 0;
}
