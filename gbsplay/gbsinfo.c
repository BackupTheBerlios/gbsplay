/* $Id: gbsinfo.c,v 1.5 2004/03/12 19:02:15 mitch Exp $
 *
 * gbsplay is a Gameboy sound player
 *
 * 2003-2004 (C) by Tobias Diedrich <ranma@gmx.at>
 *                  Christian Garbs <mitch@cgarbs.de>
 * Licensed under GNU GPL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "common.h"
#include "gbhw.h"
#include "gbcpu.h"
#include "gbs.h"

/* global variables */
char *myname;

void usage(int exitcode)
{
        FILE *out = exitcode ? stderr : stdout;
        fprintf(out,
                _("Usage: %s [option] <gbs-file>\n"
		  "\n"
		  "Available options are:\n"
		  "  -h  display this help and exit\n"
		  "  -V  print version and exit\n"),
                myname);
        exit(exitcode);
}

void version(void)
{
	puts("gbsplay " GBS_VERSION);
	exit(0);
}

void parseopts(int *argc, char ***argv)
{
	int res;
	myname = *argv[0];
	while ((res = getopt(*argc, *argv, "hV")) != -1) {
		switch (res) {
		default:
			usage(1);
			break;
		case 'h':
			usage(0);
			break;
		case 'V':
			version();
			break;
		}
	}
	*argc -= optind;
	*argv += optind;
}

int main(int argc, char **argv)
{
	struct gbs *gbs;

	i18n_init();

        parseopts(&argc, &argv);
	
        if (argc < 1) {
                usage(1);
        }

	if ((gbs = gbs_open(argv[1])) == NULL) exit(1);
	gbs_printinfo(gbs, 1);
	gbs_close(gbs);

	return 0;
}
