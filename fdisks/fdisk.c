/* fdisk.c -- Partition table manipulator for Linux.
 *
 * Copyright (C) 1992  A. V. Le Blanc (LeBlanc@mcc.ac.uk)
 * Copyright (C) 2012  Davidlohr Bueso <dave@gnu.org>
 *
 * This program is free software.  You can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation: either version 1 or
 * (at your option) any later version.
 *
 * Modified by dev@breeze.tsert.com 2013/05/17
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <limits.h>

#include "xalloc.h"
#include "nls.h"
#include "rpmatch.h"
#include "blkdev.h"
#include "common.h"
#include "mbsalign.h"
#include "fdisk.h"
#include "wholedisk.h"
#include "pathnames.h"
#include "canonicalize.h"
#include "strutils.h"
#include "closestream.h"

#include "fdisksunlabel.h"
#include "fdisksgilabel.h"
#include "fdiskmaclabel.h"
#include "fdiskdoslabel.h"
#include "fdiskbsdlabel.h"

#include "nls.h"
#include "xalloc.h"
#include "randutils.h"
#include "common.h"

#ifdef HAVE_LINUX_COMPILER_H
#include <linux/compiler.h>
#endif
#ifdef HAVE_LINUX_BLKPG_H
#include <linux/blkpg.h>
#endif

#define set_hsc(h,s,c,sector) { \
	        s = sector % cxt->geom.sectors + 1;         \
	        sector /= cxt->geom.sectors;                \
	        h = sector % cxt->geom.heads;               \
	        sector /= cxt->geom.heads;              \
	        c = sector & 0xff;                  \
	        s |= (sector >> 2) & 0xc0;              \
	    }


#define sector(s)   ((s) & 0x3f)
#define cylinder(s, c)  ((c) | (((s) & 0xc0) << 2))

#define alignment_required(_x)  ((_x)->grain != (_x)->sector_size)

#define EXTENDED_PARTITION	(0x05)

/* menu list description */

struct menulist_descr {
	char command;				/* command key */
	char *description;			/* command description */
	enum fdisk_labeltype label[2];		/* disklabel types associated with main and expert menu */
};

static const struct menulist_descr menulist[] = {
	{'a', N_("change number of alternate cylinders"), {0, FDISK_DISKLABEL_SUN}},
	{'a', N_("select bootable partition"), {FDISK_DISKLABEL_SGI, 0}},
	{'a', N_("toggle a bootable flag"), {FDISK_DISKLABEL_DOS, 0}},
	{'a', N_("toggle a read only flag"), {FDISK_DISKLABEL_SUN, 0}},
	{'b', N_("edit bootfile entry"), {FDISK_DISKLABEL_SGI, 0}},
	{'b', N_("edit bsd disklabel"), {FDISK_DISKLABEL_DOS, 0}},
	{'b', N_("move beginning of data in a partition"), {0, FDISK_DISKLABEL_DOS}},
	{'c', N_("change number of cylinders"), {0, FDISK_DISKLABEL_DOS | FDISK_DISKLABEL_SUN}},
	{'c', N_("select sgi swap partition"), {FDISK_DISKLABEL_SGI, 0}},
	{'c', N_("toggle the dos compatibility flag"), {FDISK_DISKLABEL_DOS, 0}},
	{'c', N_("toggle the mountable flag"), {FDISK_DISKLABEL_SUN, 0}},
	{'d', N_("delete a partition"), {FDISK_DISKLABEL_DOS | FDISK_DISKLABEL_SUN | FDISK_DISKLABEL_SGI | FDISK_DISKLABEL_OSF | FDISK_DISKLABEL_GPT, 0}},
	{'d', N_("print the raw data in the partition table"), {0, FDISK_DISKLABEL_ANY}},
	{'e', N_("change number of extra sectors per cylinder"), {0, FDISK_DISKLABEL_SUN}},
	{'e', N_("edit drive data"), {FDISK_DISKLABEL_OSF, 0}},
	{'e', N_("list extended partitions"), {0, FDISK_DISKLABEL_DOS}},
	{'f', N_("fix partition order"), {0, FDISK_DISKLABEL_DOS}},
	{'g', N_("create a new empty GPT partition table"), {~FDISK_DISKLABEL_OSF, 0}},
	{'g', N_("create an IRIX (SGI) partition table"), {0, FDISK_DISKLABEL_ANY}}, /* for backward compatibility only */
	{'G', N_("create an IRIX (SGI) partition table"), {~FDISK_DISKLABEL_OSF, 0}},
	{'h', N_("change number of heads"), {0, FDISK_DISKLABEL_DOS | FDISK_DISKLABEL_SUN}},
	{'i', N_("change interleave factor"), {0, FDISK_DISKLABEL_SUN}},
	{'i', N_("change the disk identifier"), {0, FDISK_DISKLABEL_DOS}},
	{'i', N_("install bootstrap"), {FDISK_DISKLABEL_OSF, 0}},
	{'l', N_("list known partition types"), {FDISK_DISKLABEL_DOS | FDISK_DISKLABEL_SUN | FDISK_DISKLABEL_SGI | FDISK_DISKLABEL_OSF  | FDISK_DISKLABEL_GPT, 0}},
	{'m', N_("print this menu"), {FDISK_DISKLABEL_ANY, FDISK_DISKLABEL_ANY}},
	{'n', N_("add a new partition"), {FDISK_DISKLABEL_DOS | FDISK_DISKLABEL_SUN | FDISK_DISKLABEL_SGI | FDISK_DISKLABEL_OSF | FDISK_DISKLABEL_GPT, 0}},
	{'o', N_("change rotation speed (rpm)"), {0, FDISK_DISKLABEL_SUN}},
	{'o', N_("create a new empty DOS partition table"), {~FDISK_DISKLABEL_OSF, 0}},
	{'p', N_("print the partition table"), {FDISK_DISKLABEL_DOS | FDISK_DISKLABEL_SUN | FDISK_DISKLABEL_SGI | FDISK_DISKLABEL_OSF, FDISK_DISKLABEL_DOS | FDISK_DISKLABEL_SUN}},
	{'q', N_("quit without saving changes"), {FDISK_DISKLABEL_ANY, FDISK_DISKLABEL_ANY}},
	{'r', N_("return to main menu"), {FDISK_DISKLABEL_OSF, FDISK_DISKLABEL_DOS | FDISK_DISKLABEL_SUN | FDISK_DISKLABEL_SGI | FDISK_DISKLABEL_OSF}},
	{'s', N_("change number of sectors/track"), {0, FDISK_DISKLABEL_DOS | FDISK_DISKLABEL_SUN}},
	{'s', N_("create a new empty Sun disklabel"), {~FDISK_DISKLABEL_OSF, 0}},
	{'s', N_("show complete disklabel"), {FDISK_DISKLABEL_OSF, 0}},
	{'t', N_("change a partition's system id"), {FDISK_DISKLABEL_DOS | FDISK_DISKLABEL_SUN | FDISK_DISKLABEL_SGI | FDISK_DISKLABEL_OSF, 0}},
	{'u', N_("change display/entry units"), {FDISK_DISKLABEL_DOS | FDISK_DISKLABEL_SUN | FDISK_DISKLABEL_SGI | FDISK_DISKLABEL_OSF, 0}},
	{'v', N_("verify the partition table"), {FDISK_DISKLABEL_DOS | FDISK_DISKLABEL_SUN | FDISK_DISKLABEL_SGI, FDISK_DISKLABEL_DOS | FDISK_DISKLABEL_SUN | FDISK_DISKLABEL_SGI}},
	{'w', N_("write disklabel to disk"), {FDISK_DISKLABEL_OSF, 0 }},
	{'w', N_("write table to disk and exit"), {FDISK_DISKLABEL_DOS | FDISK_DISKLABEL_SUN | FDISK_DISKLABEL_SGI  | FDISK_DISKLABEL_GPT, FDISK_DISKLABEL_DOS | FDISK_DISKLABEL_SUN | FDISK_DISKLABEL_SGI}},
	{'x', N_("extra functionality (experts only)"), {FDISK_DISKLABEL_DOS | FDISK_DISKLABEL_SUN | FDISK_DISKLABEL_SGI, 0}},
#if !defined (__alpha__)
	{'x', N_("link BSD partition to non-BSD partition"), {FDISK_DISKLABEL_OSF, 0}},
#endif
	{'y', N_("change number of physical cylinders"), {0, FDISK_DISKLABEL_SUN}},
};


sector_t get_nr_sects(struct partition *p) {
	return read4_little_endian(p->size4);
}

char *line_ptr, /* interactive input */
	line_buffer[LINE_LENGTH];

int	nowarn = 0; /* no warnings for fdisk -l/-s */

unsigned int user_cylinders, user_heads, user_sectors;

void toggle_units(struct fdisk_context *cxt)
{
	fdisk_context_set_unit(cxt,
		fdisk_context_use_cylinders(cxt) ? "sectors" : "cylinders");
	if (fdisk_context_use_cylinders(cxt))
		fdisk_info(cxt, _("Changing display/entry units to cylinders (DEPRECATED!)."));
	else {
		fdisk_info(cxt, _("Changing display/entry units to sectors."));
	}
}

static void __attribute__ ((__noreturn__)) usage(FILE *out)
{
	fprintf(out, _("Usage:\n"
		       " %1$s [options] <disk>    change partition table\n"
		       " %1$s [options] -l <disk> list partition table(s)\n"
		       " %1$s -s <partition>      give partition size(s) in blocks\n"
		       "\nOptions:\n"
		       " -b <size>             sector size (512, 1024, 2048 or 4096)\n"
		       " -c[=<mode>]           compatible mode: 'dos' or 'nondos' (default)\n"
		       " -h                    print this help text\n"
		       " -u[=<unit>]           display units: 'cylinders' or 'sectors' (default)\n"
		       " -v                    print program version\n"
		       " -C <number>           specify the number of cylinders\n"
		       " -H <number>           specify the number of heads\n"
		       " -S <number>           specify the number of sectors per track\n"
		       " -A <sector>           specify the alignment in sectors [2048]\n"
		       " -B <input>            specify the sfdisk-like batch file\n"
		       "\n"), program_invocation_short_name);
	exit(out == stderr ? EXIT_FAILURE : EXIT_SUCCESS);
}

void __attribute__((__noreturn__))
fatal(struct fdisk_context *cxt, enum failure why)
{
	close(cxt->dev_fd);
	switch (why) {
		case unable_to_read:
			err(EXIT_FAILURE, _("unable to read %s"), cxt->dev_path);

		case unable_to_seek:
			err(EXIT_FAILURE, _("unable to seek on %s"), cxt->dev_path);

		case unable_to_write:
			err(EXIT_FAILURE, _("unable to write %s"), cxt->dev_path);

		case ioctl_error:
			err(EXIT_FAILURE, _("BLKGETSIZE ioctl failed on %s"), cxt->dev_path);

		default:
			err(EXIT_FAILURE, _("fatal error"));
	}
}

struct partition *
get_part_table(int i) {
	return ptes[i].part_table;
}

void print_menu(struct fdisk_context *cxt, enum menutype menu)
{
	size_t i;
	int id;

	puts(_("Command action"));

	id = cxt && cxt->label ?  cxt->label->id : FDISK_DISKLABEL_ANY;

	for (i = 0; i < ARRAY_SIZE(menulist); i++)
		if (menulist[i].label[menu] & id)
			printf("   %c   %s\n", menulist[i].command, menulist[i].description);
}

void list_partition_types(struct fdisk_context *cxt)
{
	struct fdisk_parttype *types;
	size_t ntypes = 0;

	if (!cxt || !cxt->label || !cxt->label->parttypes)
		return;

	types = cxt->label->parttypes;
	ntypes = cxt->label->nparttypes;

	if (types[0].typestr == NULL) {
		/*
		 * Prints in 4 columns in format <hex> <name>
		 */
		size_t last[4], done = 0, next = 0, size;
		int i;

		size = ntypes;
		if (types[ntypes - 1].name == NULL)
			size--;

		for (i = 3; i >= 0; i--)
			last[3 - i] = done += (size + i - done) / (i + 1);
		i = done = 0;

		do {
			#define NAME_WIDTH 15
			char name[NAME_WIDTH * MB_LEN_MAX];
			size_t width = NAME_WIDTH;
			struct fdisk_parttype *t = &types[next];
			size_t ret;

			if (t->name) {
				printf("%c%2x  ", i ? ' ' : '\n', t->type);
				ret = mbsalign(_(t->name), name, sizeof(name),
						      &width, MBS_ALIGN_LEFT, 0);

				if (ret == (size_t)-1 || ret >= sizeof(name))
					printf("%-15.15s", _(t->name));
				else
					fputs(name, stdout);
			}

			next = last[i++] + done;

			if (i > 3 || next >= last[i]) {
				i = 0;
				next = ++done;
			}
		} while (done < last[0]);

	} else {
		/*
		 * Prints 1 column in format <idx> <name> <typestr>
		 */
		struct fdisk_parttype *t;
		size_t i;

		for (i = 0, t = types; t && i < ntypes; t++, i++) {
			if (t->name)
				printf("%3zu %-30s %s\n", i + 1,
						t->name, t->typestr);
		}
	}
	putchar('\n');
}

static int
test_c(char **m, char *mesg) {
	int val = 0;
	if (!*m)
		fprintf(stderr, _("You must set"));
	else {
		fprintf(stderr, " %s", *m);
		val = 1;
	}
	*m = mesg;
	return val;
}

int warn_geometry(struct fdisk_context *cxt)
{
	char *m = NULL;
	int prev = 0;

	if (fdisk_is_disklabel(cxt, SGI)) /* cannot set cylinders etc anyway */
		return 0;

	if (!cxt->geom.heads)
		prev = test_c(&m, _("heads"));

	if (!cxt->geom.sectors)
		prev = test_c(&m, _("sectors"));

	if (!cxt->geom.cylinders)
		prev = test_c(&m, _("cylinders"));

	if (!m)
		return 0;
	fprintf(stderr,
		_("%s%s.\nYou can do this from the extra functions menu.\n"),
		prev ? _(" and ") : " ", m);
	return 1;
}

void warn_limits(struct fdisk_context *cxt)
{
	if (cxt->total_sectors > UINT_MAX && !nowarn) {
		unsigned long long bytes = cxt->total_sectors * cxt->sector_size;
		int giga = bytes / 1000000000;
		int hectogiga = (giga + 50) / 100;

		fprintf(stderr, _("\n"
"WARNING: The size of this disk is %d.%d TB (%llu bytes).\n"
"DOS partition table format can not be used on drives for volumes\n"
"larger than (%llu bytes) for %ld-byte sectors. Use parted(1) and GUID \n"
"partition table format (GPT).\n\n"),
			hectogiga / 10, hectogiga % 10,
			bytes,
			(sector_t ) UINT_MAX * cxt->sector_size,
			cxt->sector_size);
	}
}

static void maybe_exit(struct fdisk_context *cxt, int rc, int *asked)
{
	char line[LINE_LENGTH];

	assert(cxt);
	assert(cxt->label);

	putchar('\n');
	if (asked)
		*asked = 0;

	if (fdisk_label_is_changed(cxt->label)) {
		fprintf(stderr, _("Do you really want to quit? "));

		if (!fgets(line, LINE_LENGTH, stdin) || rpmatch(line) == 1)
			goto leave;
		if (asked)
			*asked = 1;
		return;
	}
leave:
	fdisk_free_context(cxt);
	exit(rc);
}

/* read line; return 0 or first char */
int read_line(struct fdisk_context *cxt, int *asked)
{
	line_ptr = line_buffer;
	if (!fgets(line_buffer, LINE_LENGTH, stdin)) {
		maybe_exit(cxt, 1, asked);
		return 0;
	}
	if (asked)
		*asked = 0;
	while (*line_ptr && !isgraph(*line_ptr))
		line_ptr++;
	return *line_ptr;
}

char read_char(struct fdisk_context *cxt, char *mesg)
{
	do {
		fputs(mesg, stdout);
		fflush (stdout);	 /* requested by niles@scyld.com */

	} while (!read_line(cxt, NULL));

	return *line_ptr;
}

char read_chars(struct fdisk_context *cxt, char *mesg)
{
	int rc, asked = 0;

	do {
	        fputs(mesg, stdout);
		fflush (stdout);	/* niles@scyld.com */
		rc = read_line(cxt, &asked);
	} while (asked);

	if (!rc) {
		*line_ptr = '\n';
		line_ptr[1] = 0;
	}
	return *line_ptr;
}

struct fdisk_parttype *read_partition_type(struct fdisk_context *cxt)
{
	if (!cxt || !cxt->label || !cxt->label->nparttypes)
		return NULL;

        do {
		size_t sz;

		if (cxt->label->parttypes[0].typestr)
			read_chars(cxt, _("Partition type (type L to list all types): "));
		else
			read_chars(cxt, _("Hex code (type L to list all codes): "));

		sz = strlen(line_ptr);
		if (!sz || line_ptr[sz - 1] != '\n' || sz == 1)
			continue;
		line_ptr[sz - 1] = '\0';

		if (tolower(*line_ptr) == 'l')
			list_partition_types(cxt);
		else
			return fdisk_parse_parttype(cxt, line_ptr);
        } while (1);

	return NULL;
}


unsigned int
read_int_with_suffix(struct fdisk_context *cxt,
	unsigned int low, unsigned int dflt, unsigned int high,
	 unsigned int base, char *mesg, int *is_suffix_used)
{
	unsigned int res;
	int default_ok = 1;
	int absolute = 0;
	static char *ms = NULL;
	static size_t mslen = 0;

	if ( cxt->batch.enabled ) {

		struct fdisk_batch_record *rec =
			cxt->batch.lines[ cxt->batch.curline ];

		long long value = rec->start + rec->size;

	 	if ( is_suffix_used ) { (*is_suffix_used) = 0; }

		if (value >= low && value <= high) {
			/* fprintf( stderr, _("LOW value=%lld %lld\n" ), value, rec->size ); */
			return value;
		}

		/* fprintf( stderr, _("LOW - low=%d %lld\n" ), low, rec->size ); */
		return low + rec->size;
	}

	if ( !ms || strlen(mesg)+100 > mslen ) {
		mslen = strlen(mesg)+200;
		ms = xrealloc(ms,mslen);
	}

	if (dflt < low || dflt > high)
		default_ok = 0;

	if (default_ok)
		snprintf(ms, mslen, _("%s (%u-%u, default %u): "),
			 mesg, low, high, dflt);
	else
		snprintf(ms, mslen, "%s (%u-%u): ",
			 mesg, low, high);

	while (1) {
		int use_default = default_ok;

		/* ask question and read answer */
		while (read_chars(cxt, ms) != '\n' && !isdigit(*line_ptr)
		       && *line_ptr != '-' && *line_ptr != '+')
			continue;

		if (*line_ptr == '+' || *line_ptr == '-') {
			int minus = (*line_ptr == '-');
			int suflen;

			absolute = 0;
			res = atoi(line_ptr + 1);

			while (isdigit(*++line_ptr))
				use_default = 0;

			while (isspace(*line_ptr))
				line_ptr++;

			suflen = strlen(line_ptr) - 1;

			while(isspace(*(line_ptr + suflen)))
				*(line_ptr + suflen--) = '\0';

			if ((*line_ptr == 'C' || *line_ptr == 'c') &&
			    *(line_ptr + 1) == '\0') {
				/*
				 * Cylinders
				 */
				if (fdisk_context_use_cylinders(cxt))
					res *= cxt->geom.heads * cxt->geom.sectors;

			} else if (*line_ptr &&
				   *(line_ptr + 1) == 'B' &&
				   *(line_ptr + 2) == '\0') {
				/*
				 * 10^N
				 */
				if (*line_ptr == 'K')
					absolute = 1000;
				else if (*line_ptr == 'M')
					absolute = 1000000;
				else if (*line_ptr == 'G')
					absolute = 1000000000;
				else
					absolute = -1;
			} else if (*line_ptr &&
				   *(line_ptr + 1) == '\0') {
				/*
				 * 2^N
				 */
				if (*line_ptr == 'K')
					absolute = 1 << 10;
				else if (*line_ptr == 'M')
					absolute = 1 << 20;
				else if (*line_ptr == 'G')
					absolute = 1 << 30;
				else
					absolute = -1;
			} else if (*line_ptr != '\0')
				absolute = -1;

			if (absolute == -1)  {
				printf(_("Unsupported suffix: '%s'.\n"), line_ptr);
				printf(_("Supported: 10^N: KB (KiloByte), MB (MegaByte), GB (GigaByte)\n"
					 "            2^N: K  (KibiByte), M  (MebiByte), G  (GibiByte)\n"));
				continue;
			}

			if (absolute && res) {
				unsigned long long bytes;
				unsigned long unit;

				bytes = (unsigned long long) res * absolute;
				unit = cxt->sector_size * fdisk_context_get_units_per_sector(cxt);
				bytes += unit/2;	/* round */
				bytes /= unit;
				res = bytes;
			}
			if (minus)
				res = -res;
			res += base;
		} else {
			res = atoi(line_ptr);
			while (isdigit(*line_ptr)) {
				line_ptr++;
				use_default = 0;
			}
		}
		if (use_default) {
			printf(_("Using default value %u\n"), dflt);
			return dflt;
		}
		if (res >= low && res <= high)
			break;
		else
			printf(_("Value out of range.\n"));
	}
	if (is_suffix_used)
			*is_suffix_used = absolute > 0;
	return res;
}

/*
 * Print the message MESG, then read an integer in LOW..HIGH.
 * If the user hits Enter, DFLT is returned, provided that is in LOW..HIGH.
 * Answers like +10 are interpreted as offsets from BASE.
 *
 * There is no default if DFLT is not between LOW and HIGH.
 */
unsigned int
read_int(struct fdisk_context *cxt,
	unsigned int low, unsigned int dflt, unsigned int high,
	unsigned int base, char *mesg)
{
	if ( cxt->batch.enabled ) {

		struct fdisk_batch_record *rec =
			cxt->batch.lines[ cxt->batch.curline ];

		long long value = rec->start;

		if ( cxt->batch.curline > 0 )
			value = value * 8 / 8;

		/*
		fprintf( stderr,
			_("low=%d dflt=%d hi=%d base=%d start=%lld value=%lld\n" ),
			low, dflt, high, base, rec->start, value
		);
		*/

		if (value >= low && value <= high) {
			rec->start = value;
			return value;
		}

		value = low;
		value = value * 8 / 8;
		rec->start = value;

		/* fprintf( stderr, _("LOW value=%lld %lld\n" ), value, rec->start ); */
		return value;
	}
	return read_int_with_suffix(cxt, low, dflt, high, base, mesg, NULL);
}

static void toggle_dos_compatibility_flag(struct fdisk_context *cxt)
{
	struct fdisk_label *lb = fdisk_context_get_label(cxt, "dos");
	int flag;

	if (!lb)
		return;

	flag = !fdisk_dos_is_compatible(lb);

	if (flag)
		printf(_("DOS Compatibility flag is set (DEPRECATED!)\n"));
	else
		printf(_("DOS Compatibility flag is not set\n"));

	fdisk_dos_enable_compatible(lb, flag);

	if (fdisk_is_disklabel(cxt, DOS))
		fdisk_reset_alignment(cxt);
}

static void delete_partition(struct fdisk_context *cxt, int partnum)
{
	if (partnum < 0 || warn_geometry(cxt))
		return;

	ptes[partnum].changed = 1;

	if (fdisk_delete_partition(cxt, partnum) != 0)
		printf(_("Could not delete partition %d\n"), partnum + 1);
	else {
		printf(_("Partition %d is deleted\n"), partnum + 1);
	}
}

static void change_partition_type(struct fdisk_context *cxt)
{
	size_t i;
	struct fdisk_parttype *t, *org_t;

	assert(cxt);
	assert(cxt->label);

	if (fdisk_ask_partnum(cxt, &i, FALSE))
		return;

	org_t = t = fdisk_get_partition_type(cxt, i);
	if (!t)
                printf(_("Partition %zu does not exist yet!\n"), i + 1);

        else do {
		t = read_partition_type(cxt);
		if (!t)
			continue;

		if (fdisk_set_partition_type(cxt, i, t) == 0) {
			ptes[i].changed = 1;
			printf (_("Changed type of partition '%s' to '%s'\n"),
				org_t ? org_t->name : _("Unknown"),
				    t ?     t->name : _("Unknown"));
		} else {
			printf (_("Type of partition %zu is unchanged: %s\n"),
				i + 1,
				org_t ? org_t->name : _("Unknown"));
		}
		break;
        } while (1);

	fdisk_free_parttype(t);
	fdisk_free_parttype(org_t);
}

static void
list_disk_geometry(struct fdisk_context *cxt) {
	unsigned long long bytes = cxt->total_sectors * cxt->sector_size;
	long megabytes = bytes/1000000;

	if (megabytes < 10000)
		printf(_("\nDisk %s: %ld MB, %lld bytes"),
		       cxt->dev_path, megabytes, bytes);
	else {
		long hectomega = (megabytes + 50) / 100;
		printf(_("\nDisk %s: %ld.%ld GB, %llu bytes"),
		       cxt->dev_path, hectomega / 10, hectomega % 10, bytes);
	}
	printf(_(", %llu sectors\n"), cxt->total_sectors);
	if (is_dos_compatible(cxt))
		printf(_("%d heads, %llu sectors/track, %llu cylinders\n"),
		       cxt->geom.heads, cxt->geom.sectors, cxt->geom.cylinders);
	printf(_("Units = %s of %d * %ld = %ld bytes\n"),
	       fdisk_context_get_unit(cxt, PLURAL),
	       fdisk_context_get_units_per_sector(cxt),
	       cxt->sector_size,
	       fdisk_context_get_units_per_sector(cxt) * cxt->sector_size);

	printf(_("Sector size (logical/physical): %lu bytes / %lu bytes\n"),
				cxt->sector_size, cxt->phy_sector_size);
	printf(_("I/O size (minimum/optimal): %lu bytes / %lu bytes\n"),
				cxt->min_io_size, cxt->io_size);
	if (cxt->alignment_offset)
		printf(_("Alignment offset: %lu bytes\n"), cxt->alignment_offset);
	if (fdisk_dev_has_disklabel(cxt))
		printf(_("Disk label type: %s\n"), cxt->label->name);
	if (fdisk_is_disklabel(cxt, DOS))
		dos_print_mbr_id(cxt);
	printf("\n");
}

static void list_table(struct fdisk_context *cxt, int xtra)
{
	if (fdisk_is_disklabel(cxt, SUN)) {
		sun_list_table(cxt, xtra);
		return;
	}

	if (fdisk_is_disklabel(cxt, SGI)) {
		sgi_list_table(cxt, xtra);
		return;
	}

	list_disk_geometry(cxt);

	if (fdisk_is_disklabel(cxt, GPT)) {
		gpt_list_table(cxt, xtra);
		return;
	}

	if (fdisk_is_disklabel(cxt, OSF)) {
		xbsd_print_disklabel(cxt, xtra);
		return;
	}

	if (fdisk_is_disklabel(cxt, DOS))
		dos_list_table(cxt, xtra);
}

static void verify(struct fdisk_context *cxt)
{
	if (warn_geometry(cxt))
		return;

	fdisk_verify_disklabel(cxt);
}

void print_partition_size(struct fdisk_context *cxt,
			  int num, sector_t start, sector_t stop, int sysid)
{
	char *str = size_to_human_string(SIZE_SUFFIX_3LETTER | SIZE_SUFFIX_SPACE,
				     (uint64_t)(stop - start + 1) * cxt->sector_size);
	struct fdisk_parttype *t = fdisk_get_parttype_from_code(cxt, sysid);

	printf(_("Partition %d of type %s and of size %s is set\n"),
			num, t ? t->name : _("Unknown"), str);
	free(str);
}

static void new_partition(struct fdisk_context *cxt, struct fdisk_parttype *t)
{
	assert(cxt);
	assert(cxt->label);

	if (warn_geometry(cxt))
		return;

	fdisk_add_partition( cxt, t );
}

static void write_table(struct fdisk_context *cxt)
{
	int rc;

	rc = fdisk_write_disklabel(cxt);
	if (rc)
		err(EXIT_FAILURE, _("cannot write disk label"));

	printf(_("The partition table has been altered!\n\n"));
	reread_partition_table(cxt, 1);
}

void
reread_partition_table(struct fdisk_context *cxt, int leave) {
	int i;
	struct stat statbuf;

	i = fstat(cxt->dev_fd, &statbuf);
	if (i == 0 && S_ISBLK(statbuf.st_mode)) {
		sync();
#ifdef BLKRRPART
		printf(_("Calling ioctl() to re-read partition table.\n"));
		i = ioctl(cxt->dev_fd, BLKRRPART);
#else
		errno = ENOSYS;
		i = 1;
#endif
        }

	if (i) {
		printf(_("\nWARNING: Re-reading the partition table failed with error %d: %m.\n"
			 "The kernel still uses the old table. The new table will be used at\n"
			 "the next reboot or after you run partprobe(8) or kpartx(8)\n"),
			errno);
	}

	if (leave) {
		if (fsync(cxt->dev_fd) || close(cxt->dev_fd)) {
			fprintf(stderr, _("\nError closing file\n"));
			exit(1);
		}

		printf(_("Syncing disks.\n"));
		sync();
		exit(!!i);
	}
}

#define MAX_PER_LINE	16
static void
print_buffer(struct fdisk_context *cxt, unsigned char pbuffer[]) {
	unsigned int i, l;

	for (i = 0, l = 0; i < cxt->sector_size; i++, l++) {
		if (l == 0)
			printf("0x%03X:", i);
		printf(" %02X", pbuffer[i]);
		if (l == MAX_PER_LINE - 1) {
			printf("\n");
			l = -1;
		}
	}
	if (l > 0)
		printf("\n");
	printf("\n");
}

static void print_raw(struct fdisk_context *cxt)
{
	size_t i;

	assert(cxt);
	assert(cxt->label);

	printf(_("Device: %s\n"), cxt->dev_path);

	if (fdisk_is_disklabel(cxt, SUN) ||
	    fdisk_is_disklabel(cxt, SGI) ||
	    fdisk_is_disklabel(cxt, GPT))
	{
		print_buffer(cxt, cxt->firstsector);
	}
	else {
		for (i = 3; i < cxt->label->nparts_max; i++)
		     print_buffer(cxt, ptes[i].sectorbuffer);
	}
}

static void __attribute__ ((__noreturn__)) handle_quit(struct fdisk_context *cxt)
{
	fdisk_free_context(cxt);
	printf("\n");
	exit(EXIT_SUCCESS);
}

static void expert_command_prompt(struct fdisk_context *cxt)
{
	int c = 0;
	size_t n;

	assert(cxt);

	while (1) {
		if ( cxt->batch.enabled ) {
			/* do sfdisk processing ... */
		}
		else {
			assert(cxt->label);
			putchar('\n');
			c = tolower(read_char(cxt, _("Expert command (m for help): ")));
		}

		switch (c) {
		case 'a':
			if (fdisk_is_disklabel(cxt, SUN))
				fdisk_sun_set_alt_cyl(cxt);
			break;
		case 'b':
			if (fdisk_is_disklabel(cxt, DOS) &&
			    fdisk_ask_partnum(cxt, &n, FALSE) == 0)
				dos_move_begin(cxt, n);
			break;
		case 'c':
			user_cylinders = read_int(cxt, 1, cxt->geom.cylinders, 1048576, 0, _("Number of cylinders"));
			fdisk_override_geometry(cxt, user_cylinders, user_heads, user_sectors);
			if (fdisk_is_disklabel(cxt, SUN))
				fdisk_sun_set_ncyl(cxt, cxt->geom.cylinders);
			break;
		case 'd':
			print_raw(cxt);
			break;
		case 'e':
			if (fdisk_is_disklabel(cxt, SGI))
				sgi_set_xcyl();
			else if (fdisk_is_disklabel(cxt, SUN))
				fdisk_sun_set_xcyl(cxt);
			else
				if (fdisk_is_disklabel(cxt, DOS))
					dos_list_table_expert(cxt, 1);
			break;
		case 'f':
			if (fdisk_is_disklabel(cxt, DOS))
				dos_fix_partition_table_order(cxt);
			break;
		case 'g':
			fdisk_create_disklabel(cxt, "sgi");
			break;
		case 'h':
			user_heads = read_int(cxt, 1, cxt->geom.heads, 256, 0, _("Number of heads"));
			fdisk_override_geometry(cxt, user_cylinders, user_heads, user_sectors);
			break;
		case 'i':
			if (fdisk_is_disklabel(cxt, SUN))
				fdisk_sun_set_ilfact(cxt);
			else if (fdisk_is_disklabel(cxt, DOS))
				dos_set_mbr_id(cxt);
			break;
		case 'o':
			if (fdisk_is_disklabel(cxt, SUN))
				fdisk_sun_set_rspeed(cxt);
			break;
		case 'p':
			if (fdisk_is_disklabel(cxt, SUN))
				list_table(cxt, 1);
			else
				dos_list_table_expert(cxt, 0);
			break;
		case 'q':
			handle_quit(cxt);
		case 'r':
			return;
		case 's':
			user_sectors = read_int(cxt, 1, cxt->geom.sectors, 63, 0, _("Number of sectors"));
			if (is_dos_compatible(cxt))
				fprintf(stderr, _("Warning: setting "
					"sector offset for DOS "
					"compatibility\n"));
			fdisk_override_geometry(cxt, user_cylinders, user_heads, user_sectors);
			break;
		case 'v':
			verify(cxt);
			break;
		case 'w':
			write_table(cxt);
			break;
		case 'y':
			if (fdisk_is_disklabel(cxt, SUN))
				fdisk_sun_set_pcylcount(cxt);
			break;
		default:
			print_menu(cxt, EXPERT_MENU);
		}
	}
}

static int is_ide_cdrom_or_tape(char *device)
{
	int fd, ret;

	if ((fd = open(device, O_RDONLY)) < 0)
		return 0;
	ret = blkdev_is_cdrom(fd);

	close(fd);
	return ret;
}

/* Print disk geometry and partition table of a specified device (-l option) */
static void print_partition_table_from_option(struct fdisk_context *cxt,
				char *device, unsigned long sector_size)
{
	if (fdisk_context_assign_device(cxt, device, 1) != 0)	/* read-only */
		err(EXIT_FAILURE, _("cannot open %s"), device);

	if (sector_size) /* passed -b option, override autodiscovery */
		fdisk_override_sector_size(cxt, sector_size);

	if (user_cylinders || user_heads || user_sectors)
		fdisk_override_geometry(cxt, user_cylinders,
					user_heads, user_sectors);

	if (fdisk_dev_has_disklabel(cxt))
		list_table(cxt, 0);
	else
		list_disk_geometry(cxt);
}

/*
 * for fdisk -l:
 * try all things in /proc/partitions that look like a full disk
 */
static void
print_all_partition_table_from_option(struct fdisk_context *cxt,
				      unsigned long sector_size)
{
	FILE *procpt;
	char line[128 + 1], ptname[128 + 1], devname[256];
	int ma, mi;
	unsigned long long sz;

	procpt = fopen(_PATH_PROC_PARTITIONS, "r");
	if (procpt == NULL) {
		fprintf(stderr, _("cannot open %s\n"), _PATH_PROC_PARTITIONS);
		return;
	}

	while (fgets(line, sizeof(line), procpt)) {
		if (sscanf (line, " %d %d %llu %128[^\n ]",
			    &ma, &mi, &sz, ptname) != 4)
			continue;
		snprintf(devname, sizeof(devname), "/dev/%s", ptname);
		if (is_whole_disk(devname)) {
			char *cn = canonicalize_path(devname);
			if (cn) {
				if (!is_ide_cdrom_or_tape(cn))
					print_partition_table_from_option(cxt, cn, sector_size);
				free(cn);
			}
		}
	}
	fclose(procpt);
}

static void
unknown_command(int c) {
	printf(_("%c: unknown command\n"), c);
}

static void print_welcome(void)
{
	printf(_("Welcome to fdisk (%s).\n\n"
		 "Changes will remain in memory only, until you decide to write them.\n"
		 "Be careful before using the write command.\n\n"), PACKAGE_STRING);

	fflush(stdout);
}

static int fdisk_set_parttype(struct fdisk_context *ctx, struct fdisk_parttype *t, const char *parttype)
{
	static char buffer[8]={ 'u', 'n', 'k', 'n', 'o', 'w', 'n', '\0' };
	struct fdisk_parttype *pt = 0L;

	t->type = 0;
	t->typestr = buffer;
	t->flags = FDISK_PARTTYPE_UNKNOWN;

	if (strncasecmp( parttype, "0x", 2 ) == 0)
		t->type = (int) strtol( parttype, 0L, 16 );
	else
	if (strcasecmp( parttype, "L" ) == 0)
		t->type = 0x83;
	else
	if (strcasecmp( parttype, "E" ) == 0)
		t->type = 0x05;
	else
	if (strcasecmp( parttype, "X" ) == 0)
		t->type = 0x85;
	else
	if (strcasecmp( parttype, "S" ) == 0 ||
		strcasecmp( parttype, "Swap" ) == 0)
	{
		t->type = 0x82;
	}
	else
	if (strcasecmp( parttype, "R" ) == 0 ||
		strcasecmp( parttype, "Raid" ) == 0)
	{
		t->type = 0xfd;
	}
	else
	if (strcasecmp( parttype, "V" ) == 0 ||
		strcasecmp( parttype, "LVM" ) == 0)
	{
		t->type = 0x8e;
	}
	else
	if (strcasecmp( parttype, "F" ) == 0 ||
		strcasecmp( parttype, "FAT32" ) == 0)
	{
		t->type = 0x0b;
	}
	else {
		t->type = (int) strtol( parttype, 0L, 16 );
	}

	if ( t->type ) {
		/*
		fprintf(stderr, _("Partition type %d\n"), t->type );
		*/

		if ((pt = fdisk_get_parttype_from_code( ctx, t->type ))) {
			t->type = pt->type;
			t->name = pt->name;
			t->typestr = pt->typestr;
			t->flags = pt->flags;
			/*
			fprintf(stderr, _("Partition type %0x\n"), pt->type );
			fprintf(stderr, _("Partition flags %0x\n"), pt->flags );
			fprintf(stderr, _("Partition name %s\n"), pt->name );
			fprintf(stderr, _("Partition typestr %s\n"), pt->typestr );
			*/
		}
	}
	return t->type ? 0 : (-1);
}

static int fdisk_next_batch_cmd(struct fdisk_context *ctx, struct fdisk_parttype *t)
{
	struct fdisk_batch_record *rec = ctx->batch.lines[ ctx->batch.curline ];

	/*
	if ( rec && rec->kept ) {
		ctx->batch.command = '\0';
		return 'k';
	}
	*/

	if ( ctx->batch.command == 'a' ) {
		ctx->batch.command = '\0';

		if ( rec && rec->bootable ) {
			return 'a';
		}
	}

	if ( ctx->batch.command == '\0' ) {
		ctx->batch.curline += 1;
		ctx->batch.command = 'n';
		rec = ctx->batch.lines[ ctx->batch.curline ];
	}

	if ( rec && ctx->batch.command == 'n' ) {
		/* Set the partition type, and issue a new partition command */
		memcpy( t, &( rec->pt ), sizeof( struct fdisk_parttype ));
		/*
		fprintf(stderr, _("Partition type %0x\n"), t->type );
		fprintf(stderr, _("Partition flags %0x\n"), t->flags );
		fprintf(stderr, _("Partition name %s\n"), t->name );
		fprintf(stderr, _("Partition typestr %s\n"), t->typestr );
		*/
		ctx->batch.command = 'a';
		return 'n';
	}

	/* If no more partitions, issue a write table command */
	if ( ctx->batch.command == 'n' ) {
		ctx->batch.command = 'q';
		return 'w';
	}
	/* Quit command */
	return 'q';
}

static void command_prompt(struct fdisk_context *cxt)
{
	struct fdisk_parttype t;
	size_t n;
	int c;

	assert(cxt);

	if (fdisk_is_disklabel( cxt, OSF )) {
		putchar('\n');
		/* OSF label, and no DOS label */
		printf(_("Detected an OSF/1 disklabel on %s, entering "
			 "disklabel mode.\n"), cxt->dev_path);
		bsd_command_prompt(cxt);

		/* If we return we may want to make an empty DOS label? */
		fdisk_context_switch_label(cxt, "dos");
	}

	while (1) {

		if ( cxt->batch.enabled ) {

			/* do sfdisk-like processing ...
			 * create new partition (n),
			 * keep partition (k),
			 * toggle bootable flag (a),
			 * write table (w)
			 */
			c = fdisk_next_batch_cmd( cxt, &t );

			fdisk_get_batch_partnum( cxt, &n );

			if ( !c ) {
				fprintf(stderr, _("Incorrectly specified partition %d\n"), n );
				return;
			}
		}
		else {
			assert(cxt->label);
			putchar('\n');
			c = tolower(read_char(cxt, _("Command (m for help): ")));
		}

		//fprintf(stdout, _("Processing partition %d\n"), n );

		switch (c) {
		case 'k':
			new_partition( cxt, cxt->batch.enabled ? &t : 0L );
			break;
		case 'a':
			if ( cxt->batch.enabled ) {
				fdisk_partition_toggle_flag( cxt, n, DOS_FLAG_ACTIVE );
			}
			else
			if (fdisk_is_disklabel(cxt, DOS) &&
			    fdisk_ask_partnum(cxt, &n, FALSE) == 0)
				fdisk_partition_toggle_flag(cxt, n, DOS_FLAG_ACTIVE);
			else
			if (fdisk_is_disklabel(cxt, SUN) &&
				 fdisk_ask_partnum(cxt, &n, FALSE) == 0)
				fdisk_partition_toggle_flag(cxt, n, SUN_FLAG_UNMNT);
			else
			if (fdisk_is_disklabel(cxt, SGI) &&
				 fdisk_ask_partnum(cxt, &n, FALSE) == 0)
				fdisk_partition_toggle_flag(cxt, n, SGI_FLAG_BOOT);
			else
				unknown_command(c);
			break;

		case 'b':
			if (fdisk_is_disklabel(cxt, SGI))
				sgi_set_bootfile(cxt);
			else
			if (fdisk_is_disklabel(cxt, DOS)) {
				struct fdisk_context *bsd;
				bsd = fdisk_new_nested_context(cxt, "bsd");
				if (bsd)
					bsd_command_prompt(bsd);
				fdisk_free_context(bsd);
			}
			else {
				unknown_command(c);
			}
			break;
		case 'c':
			if (fdisk_is_disklabel(cxt, DOS))
				toggle_dos_compatibility_flag(cxt);
			else
			if (fdisk_is_disklabel(cxt, SUN) &&
				 fdisk_ask_partnum(cxt, &n, FALSE) == 0)
				fdisk_partition_toggle_flag(cxt, n, SUN_FLAG_RONLY);
			else
			if (fdisk_is_disklabel(cxt, SGI) &&
				 fdisk_ask_partnum(cxt, &n, FALSE) == 0)
				fdisk_partition_toggle_flag(cxt, n, SGI_FLAG_SWAP);
			else {
				unknown_command(c);
			}
			break;
		case 'd':
			if (fdisk_ask_partnum(cxt, &n, FALSE) == 0)
				delete_partition(cxt, n);
			break;
		case 'g':
			fdisk_create_disklabel(cxt, "gpt");
			break;
		case 'G':
			fdisk_create_disklabel(cxt, "sgi");
			break;
		case 'i':
			if (fdisk_is_disklabel(cxt, SGI))
				create_sgiinfo(cxt);
			else
				unknown_command(c);
			break;
		case 'l':
			list_partition_types(cxt);
			break;
		case 'm':
			print_menu(cxt, MAIN_MENU);
			break;
		case 'n':
			if ( cxt->batch.enabled ) {
				new_partition( cxt, &t );

				switch ( t.type ) {
					case 0x05:
					case 0x82:
					case 0x83:
					case 0x85:
					break;
					default:
						if (fdisk_set_partition_type( cxt, n, &t ) == 0) {
							ptes[n].changed = 1;
						} else {
							printf(_("Could not change type of partition %d\n"), n + 1);
						}
					break;
				}
			} else {
				new_partition( cxt, 0L );
			}
			break;
		case 'o':
			fdisk_create_disklabel(cxt, "dos");
			break;
		case 'p':
			list_table(cxt, 0);
			break;
		case 'q':
			handle_quit(cxt);
		case 's':
			fdisk_create_disklabel(cxt, "sun");
			break;
		case 't':
			change_partition_type(cxt);
			break;
		case 'u':
			toggle_units(cxt);
			break;
		case 'v':
			verify(cxt);
			break;
		case 'w':
			write_table(cxt);
			break;
		case 'x':
			expert_command_prompt(cxt);
			break;
		default:
			unknown_command(c);
			print_menu(cxt, MAIN_MENU);
		}
	}
}

static sector_t get_dev_blocks(char *dev)
{
	int fd;
	sector_t size;

	if ((fd = open(dev, O_RDONLY)) < 0)
		err(EXIT_FAILURE, _("cannot open %s"), dev);
	if (blkdev_get_sectors(fd, &size) == -1) {
		close(fd);
		err(EXIT_FAILURE, _("BLKGETSIZE ioctl failed on %s"), dev);
	}
	close(fd);
	return size/2;
}

#if 0
static void fdisk_batch_set_partgeom(struct fdisk_context *cxt, struct fdisk_batch_record *rec, int partno)
{
	//unsigned long start = 0;
	//unsigned long size = 0;
	int32_t extend = 0;

	//partno < cxt->label->nparts_max)
	//
	for (int i =0; i <= partno; i++) {

		struct pte *pe = &ptes[i];
		struct partition *p = (extend ? pe->ext_pointer : pe->part_table);

		if ( ! p ) { continue; }

		fprintf( stderr, "%2zd %02x%4d%4d%5d%4d%4d%5d%11lu%11lu %02x\n",
			i + 1, p->boot_ind, p->head,
			sector(p->sector),
			cylinder(p->sector, p->cyl), p->end_head,
			sector(p->end_sector),
			cylinder(p->end_sector, p->end_cyl),
			(unsigned long) get_start_sect(p),
			(unsigned long) get_nr_sects(p), p->sys_ind);

		if ( p->sys_ind ) {
			//check_consistency(cxt, p, partno);
			fdisk_warn_alignment(cxt, get_partition_start(pe), i);
		}

		if (extend == 0 && p->sys_ind == 0x05) {
			extend = 1;
			rec->start = (unsigned long) get_start_sect(p);
		}
		else
		if ( partno == i ) {
			rec->start += (unsigned long) get_start_sect(p);
			rec->size = (unsigned long) get_nr_sects(p) - 1;
			break;
		}
		else
		if ( rec->start ) {
			rec->start += (unsigned long) get_start_sect(p);
			rec->start += (unsigned long) get_nr_sects(p);
		}
	}
}
#endif // 0

static long long fdisk_batch_partoffset(struct fdisk_context *ctx, struct fdisk_batch_record *prev, const char *entry, int partno)
{
	int automatic = strcmp( entry, "+" ) == 0;
	unsigned long start = 0;

	if ( partno == 1 ) {
		start = automatic ? 63 : strtoul( entry, 0L, 10 );
	}
	else {
		assert( prev );

/*
fprintf( stderr, _("Record=0x%x %lld\n" ), prev, prev->size );
*/

		unsigned long offset = prev->start + prev->size;

		start = automatic ? offset : strtoul( entry, 0L, 10 );

		if (prev->pt.type == EXTENDED_PARTITION)
			start = automatic ? prev->start : strtoul( entry, 0L, 10 );

		start += ctx->batch.alignment;

/*
fprintf( stderr, _("Prev start=%lld Prev size=%lld New start=%ld.\n" ), prev->start, prev->size, start );
*/
	}

	if (start >= ctx->total_sectors || (prev && start < prev->start))
		return (-1);

	return start;
}

static long long fdisk_batch_partsize(struct fdisk_context *ctx, const char *entry, long long start)
{
	long long nb_sectors = ctx->total_sectors - start - 2048;
	int automatic = strcmp( entry, "+" ) == 0;
	/*
fprintf( stderr, _("fdisk_batch_partsize start=%lld size=%lld\n" ), start, nb_sectors );
	*/
	nb_sectors = automatic ? nb_sectors : strtoul( entry, 0L, 10 );
	return nb_sectors -= 1;
}

/*----------------------------------------------------------------------------*/
static int fdisk_load_batch_entries(struct fdisk_context *ctx, char *path, unsigned long align_sectors)
/*----------------------------------------------------------------------------*/
{
	struct fdisk_batch_record *oldrec = NULL;
	struct fdisk_batch_record *rec = NULL;

	//int kepts[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
	char line[ 128 ]={ '\0' };
	char *entries[ 7 ]={ 0L };
	char *sptr = line;
	char *eptr = 0L;

	FILE *infile = 0L;
	ssize_t nread = 0;
	size_t sz = 64;
	int i = 0, j = 0;
	int keep_partno = (-1);
	int first_logical = 0;
	//int kept = 0;

	ctx->batch.enabled = 1;
	ctx->batch.alignment = align_sectors;

	// All calculations are expected to be in sectors
	// A partition offset of 2048 sectors is added
	// to the start of every partition, by default.
	//
	if (! (infile = fopen( path, "r" ))) {
		fprintf( stderr, _("Could not load batch file.\n"));
		return (-1);
	}

	//for (i=0; fgets( line, 127, infile ); i++)

	/*
	for (i = ctx->label->nparts_max-1; i >= 0; i--)
		delete_partition( ctx, i );

	ctx->label->nparts_max = 0;
	*/

	for (i=0; i<20; i++) {

		sptr = line;
		nread = getline( &sptr, &sz, infile );

		if (nread < 0)
			break;

		if (nread == 0)
			continue;

		for (j=0; entries[j]; j++) {
			free( entries[j] );
			entries[j] = 0L;
		}

		line[ --nread ] = '\0';

		if (nread < 4) {
			fprintf( stderr, _("Partition %d -- Invalid entry specified '%s'.\n"), i, line );
			goto error;
		}

		/*
		fprintf( stderr, _("Partition %d -- Entries '%s'.\n"), i, line );
		*/

		if (line[ nread-1 ] == ';') {
			ctx->batch.lines[i] = 0L;
			break;
		}

		for (sptr = line, j = 0;
			j < 5 && (eptr = strchr( sptr, ',' ));
			(sptr = eptr+1))
		{
			(*eptr) = '\0';
			entries[j++] = strlen( sptr ) ? strdup( sptr ) : strdup( "+" );
			entries[j] = 0L;
		}

		if (strlen( sptr )) {
			entries[j++] = strlen( sptr ) ? strdup( sptr ) : strdup( "+" );
			entries[j] = 0L;
		}

		if ( j < 3 || j > 4 ) {
			fprintf( stderr,
				_("Partition %d -- Wrong number of entries.\n"), i );
			goto error;
		}

		oldrec = rec;
		keep_partno = (-1);

		rec = (struct fdisk_batch_record*)
			malloc( sizeof( struct fdisk_batch_record ));

		if ( rec ) {
			//rec->kept = 0;
			rec->logical = 0;
			rec->bootable = 0;
		}
		else {
			fprintf( stderr, _("Partition %d -- Out of memory.\n"), i );
			goto error;
		}

		ctx->batch.lines[i] = rec;

		rec->partno = i;
/*
fprintf( stderr, _("Record=0x%x 0x%x\n" ), oldrec, rec );
if ( oldrec ) {
fprintf( stderr, _("Record size=%lld %lld '%s'\n" ), oldrec->size, rec->size, entries[1] );
}
*/

		if ( entries[3] ) {
			if (strcmp( sptr, "*" ) == 0)
				rec->bootable = 1;
			else
			if ((*sptr) == 'k' || (*sptr) == 'K') {
				/* partition to keep */
				keep_partno = atoi( sptr+1 ) - 1;
				/*
				kepts[ atoi( sptr+1 )-1 ] = 100;
				rec->kept = kept = 1;
				rec->start = atoi( entries[0] );
				rec->size = atoi( entries[1] );
				*/
			}
			else {
				fprintf( stderr,
					_("Partition %d -- Use '*|k' for bootable|keep flag.\n"), i );
				goto error;
			}
		}

		if (oldrec && (oldrec->logical || oldrec->pt.type == EXTENDED)) {
			if ( first_logical )
				rec->partno = oldrec->partno + 1;
			else {
				first_logical = 1;
				rec->partno = 4;
			}
		}

		if ( keep_partno >= 0 ) {
			/*
			rec->start = 0; rec->size = 0;
			fdisk_batch_set_partgeom( ctx, rec, keep_partno );
			*/
			rec->start = strtoul( entries[0], 0L, 10 );
			rec->size = strtoul( entries[1], 0L, 10 );

			if ( rec->size < rec->start || rec->size < 1 || rec->start < 1 ) {
				fprintf( stderr,
					_("Partition %d -- Must specify previous start and size\n"), i );
				goto error;
			}
		} else {
			rec->start = fdisk_batch_partoffset( ctx, oldrec, entries[0], i+1 );

			if ( rec->start < 0 ) {
				fprintf( stderr, _("Partition %d -- Invalid start sector.\n"), i );
				goto error;
			}
			rec->size = fdisk_batch_partsize( ctx, entries[1], rec->start );
		}

		if ( rec->size < 1024 ) {
			fprintf( stderr,
				_("Partition %d -- Must be > 1024 sectors.\n"), i );
			goto error;
		}

		if (fdisk_set_parttype( ctx, &( rec->pt ), entries[2] )) {
			fprintf( stderr,
				_("Partition %d -- Invalid partition type.\n"), i );
			goto error;
		}

		if (oldrec && (oldrec->logical || oldrec->pt.type == EXTENDED))
			rec->logical = 1;
	}

	ctx->batch.lines[i] = 0L;

	/*
	for (i=0; ctx->batch.lines[i]; i++) {
		printf( "Line[%d] Type=%02x Start=%lld Size=%lld\n",
			i, ctx->batch.lines[i]->pt.type,
			ctx->batch.lines[i]->start,
			ctx->batch.lines[i]->size);
	}
	*/

	for (i = ctx->label->nparts_max-1; i >= 0; i--)
		delete_partition( ctx, i );

	return 0;

error:
	for (j=0; entries[j]; j++)
		free( entries[j] );

	return (-1);
}

int main(int argc, char **argv)
{
	unsigned long sector_size = 0;
	int c, optl = 0, opts = 0;
	int align_sectors = 2048;
	struct fdisk_context *cxt;
	struct fdisk_label *lb;
	char *infile = 0L;

	setlocale( LC_ALL, "" );
	bindtextdomain(PACKAGE, LOCALEDIR);
	textdomain( PACKAGE );
	atexit(close_stdout);

	fdisk_init_debug(0);

	cxt = fdisk_new_context();

	if (!cxt)
		err(EXIT_FAILURE, _("failed to allocate libfdisk context"));

	fdisk_context_set_ask(cxt, ask_callback, NULL);

	while ((c = getopt(argc, argv, "A:B:b:c::C:hH:lsS:u::vV")) != -1) {
		switch (c) {
		case 'b':
			/* Ugly: this sector size is really per device,
			   so cannot be combined with multiple disks,
			   and te same goes for the C/H/S options.
			*/
			sector_size = strtou32_or_err(optarg, _("invalid sector size argument"));
			if (sector_size != 512 && sector_size != 1024 &&
			    sector_size != 2048 && sector_size != 4096)
				usage(stderr);
			break;
		case 'C':
			user_cylinders =  strtou32_or_err(optarg, _("invalid cylinders argument"));
			break;
		case 'c':
			if (optarg) {
				/* this setting is independent on the current
				 * actively used label */
				lb = fdisk_context_get_label(cxt, "dos");
				if (!lb)
					err(EXIT_FAILURE, _("not found DOS label driver"));
				if (strcmp(optarg, "=dos") == 0)
					fdisk_dos_enable_compatible(lb, TRUE);
				else if (strcmp(optarg, "=nondos") == 0)
					fdisk_dos_enable_compatible(lb, FALSE);
				else
					usage(stderr);
			}
			/* use default if no optarg specified */
			break;
		case 'H':
			user_heads = strtou32_or_err(optarg, _("invalid heads argument"));
			if (user_heads > 256)
				user_heads = 0;
			break;
		case 'S':
			user_sectors =  strtou32_or_err(optarg, _("invalid sectors argument"));
			if (user_sectors >= 64)
				user_sectors = 0;
			break;
		case 'l':
			optl = 1;
			break;
		case 's':
			opts = 1;
			break;
		case 'u':
			if (optarg && *optarg == '=')
				optarg++;
			if (fdisk_context_set_unit(cxt, optarg) != 0)
				usage(stderr);
			break;
		case 'A':
			align_sectors = optarg ? atoi(optarg) : 0;
			if (align_sectors != 512 && align_sectors != 1024 && align_sectors != 2048)
				usage(stderr);
			break;
		case 'B':
			infile = optarg ? strdup( optarg ) : ((char*)0);
			break;
		case 'V':
		case 'v':
			printf(UTIL_LINUX_VERSION);
			return EXIT_SUCCESS;
		case 'h':
			usage(stdout);
		default:
			usage(stderr);
		}
	}

	if (sector_size && argc-optind != 1) {
		printf(_("Warning: the -b (set sector size) option should"
			 " be used with one specified device\n"));
	}

	if ( optl ) {
		nowarn = 1;

		if (argc > optind) {
			int k;
			for (k = optind; k < argc; k++)
				print_partition_table_from_option(cxt, argv[k], sector_size);
		}
		else {
			print_all_partition_table_from_option(cxt, sector_size);
		}
		exit(EXIT_SUCCESS);
	}

	if (opts) {
		/* print partition size for one or more devices */
		int i, ndevs = argc - optind;

		if (ndevs <= 0)
			usage(stderr);

		for (i = optind; i < argc; i++) {
			if (ndevs == 1)
				printf("%llu\n", get_dev_blocks(argv[i]));
			else {
				printf("%s: %llu\n", argv[i], get_dev_blocks(argv[i]));
			}
		}
		exit(EXIT_SUCCESS);
	}

	if (argc-optind != 1)
		usage(stderr);

	if (fdisk_context_assign_device(cxt, argv[optind], 0) != 0)
		err(EXIT_FAILURE, _("cannot open %s"), argv[optind]);

	if (sector_size)	/* passed -b option, override autodiscovery */
		fdisk_override_sector_size(cxt, sector_size);

	if (user_cylinders || user_heads || user_sectors)
		fdisk_override_geometry(cxt, user_cylinders, user_heads, user_sectors);

	if ( ! infile )
		print_welcome();

	if (!fdisk_dev_has_disklabel(cxt)) {
		fprintf(stderr,
			_("Device does not contain a recognized partition table\n"));
		fdisk_create_disklabel(cxt, NULL);
	}

	if ( infile ) {
		if (fdisk_load_batch_entries( cxt, infile, align_sectors ) < 0)
			err(EXIT_FAILURE, _("Loading batch entries failed\n"));
	}

	command_prompt(cxt);

	fdisk_free_context(cxt);

	return 0;
}

