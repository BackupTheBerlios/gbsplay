/* $Id: gbsplay.c,v 1.84 2004/03/20 20:41:26 mitch Exp $
 *
 * gbsplay is a Gameboy sound player
 *
 * 2003-2004 (C) by Tobias Diedrich <ranma@gmx.at>
 *                  Christian Garbs <mitch@cgarbs.de>
 * Licensed under GNU GPL.
 */

#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <math.h>
#include <termios.h>
#include <signal.h>
#include <time.h>

#include "gbhw.h"
#include "gbcpu.h"
#include "gbs.h"
#include "cfgparser.h"
#include "util.h"

#ifdef PLUGOUT_DEVDSP
#include "plugout_devdsp.h"
#endif
#ifdef PLUGOUT_NAS
#include "plugout_nas.h"
#endif
#ifdef PLUGOUT_STDOUT
#include "plugout_stdout.h"
#endif

#define LN2 .69314718055994530941
#define MAGIC 5.78135971352465960412
#define FREQ(x) (262144 / x)
/* #define NOTE(x) ((log(FREQ(x))/LN2 - log(55)/LN2)*12 + .2) */
#define NOTE(x) ((int)((log(FREQ(x))/LN2 - MAGIC)*12 + .2))

#define MAXOCTAVE 9

/* player modes */
#define PLAYMODE_LINEAR  1
#define PLAYMODE_RANDOM  2
#define PLAYMODE_SHUFFLE 3

/* lookup tables */
static char notelookup[4*MAXOCTAVE*12];
static char vollookup[5*16];
static const char vols[5] = " -=#%";

/* global variables */
static char *myname;
static int quit = 0;
static struct termios ots;
static int *subsong_playlist;
static int subsong_playlist_idx = 0;
static int pause_mode = 0;

unsigned int random_seed;

#define DEFAULT_REFRESH_DELAY 33

static int refresh_delay = DEFAULT_REFRESH_DELAY; /* msec */

/* default values */
static int playmode = PLAYMODE_LINEAR;
static int endian = CFG_ENDIAN_NE;
static int quiet = 0;
static int rate = 44100;
static int silence_timeout = 2;
static int fadeout = 3;
static int subsong_gap = 2;
static int subsong_stop = -1;
static int subsong_timeout = 2*60;
static int redraw = false;

static char *cfgfile = ".gbsplayrc";

static int16_t samples[4096];
static struct gbhw_buffer buf = {
.data = samples,
.pos  = 0,
.len  = sizeof(samples)/sizeof(int16_t),
};

/* configuration directives */
static struct cfg_option options[] = {
	{ "rate", &rate, cfg_int },
	{ "refresh_delay", &refresh_delay, cfg_int },
	{ "quiet", &quiet, cfg_int },
	{ "endian", &endian, cfg_endian },
	{ "subsong_timeout", &subsong_timeout, cfg_int },
	{ "subsong_gap", &subsong_gap, cfg_int },
	{ "fadeout", &fadeout, cfg_int },
	{ "silence_timeout", &silence_timeout, cfg_int },
	/* playmode not implemented yet */
	{ NULL, NULL, NULL }
};

/* sound output plugins */
typedef void    regparm (*plugout_open_fn )(int endian, int rate);
typedef ssize_t regparm (*plugout_write_fn)(const void *buf, size_t count);
typedef void    regparm (*plugout_close_fn)();
void regparm no_output_plugin(int endian, int rate);
struct output_plugin {
	char *id;
	char *name;
	plugout_open_fn  open_fn;
	plugout_write_fn write_fn;
	plugout_close_fn close_fn;
};
static struct output_plugin plugouts[] = {
#ifdef PLUGOUT_NAS
	{ "nas", "NAS sound driver", &nas_open, &nas_write, &nas_close },
#endif
#ifdef PLUGOUT_DEVDSP
	{ "dsp", "/dev/dsp sound driver", &devdsp_open, &devdsp_write, &devdsp_close },
#endif
#ifdef PLUGOUT_STDOUT
	{ "stdout", "STDOUT file writer", &stdout_open, &stdout_write, &stdout_close },
#endif
	{ "", "", &no_output_plugin, NULL, NULL }
};
static char* sound_id;
static char* sound_name;
static plugout_open_fn  sound_open;
static plugout_write_fn sound_write;
static plugout_close_fn sound_close;

void regparm no_output_plugin(int endian, int rate)
{
	printf(_("No output plugins available.\n\n"));
	exit(1);
}

static regparm int getnote(int div)
{
	int n = 0;

	if (div>0) {
		n = NOTE(div);
	}

	if (n < 0) {
		n = 0;
	} else if (n >= MAXOCTAVE*12) {
		n = MAXOCTAVE-1;
	}

	return n;
}

static regparm void precalc_notes(void)
{
	int i;
	for (i=0; i<MAXOCTAVE*12; i++) {
		char *s = notelookup + 4*i;
		int n = i % 12;

		s[2] = '0' + i / 12;
		n += (n > 2) + (n > 7);
		s[0] = 'A' + (n >> 1);
		if (n & 1) {
			s[1] = '#';
		} else {
			s[1] = '-';
		}
	}
}

static regparm char *reverse_vol(char *s)
{
	static char buf[5];
	int i;

	for (i=0; i<4; i++) {
		buf[i] = s[3-i];
	}
	buf[4] = 0;

	return buf;
}

static regparm void precalc_vols(void)
{
	int i, k;
	for (k=0; k<16; k++) {
		int j;
		char *s = vollookup + 5*k;
		i = k;
		for (j=0; j<4; j++) {
			if (i>=4) {
				s[j] = vols[4];
				i -= 4;
			} else {
				s[j] = vols[i];
				i = 0;
			}
		}
	}
}

static regparm void swap_endian(struct gbhw_buffer *buf)
{
	int i;

	for (i=0; i<buf->pos; i++) {
		short x = buf->data[i];
		buf->data[i] = ((x & 0xff) << 8) | (x >> 8);
	}
}

static regparm void callback(struct gbhw_buffer *buf, void *priv)
{
	if ((is_le_machine() && endian == CFG_ENDIAN_BE) ||
	    (is_be_machine() && endian == CFG_ENDIAN_LE)) {
		swap_endian(buf);
	}
	sound_write(buf->data, buf->pos*sizeof(int16_t));
	buf->pos = 0;
}

static regparm int *setup_playlist(int songs)
/* setup a playlist in shuffle mode */
{
	int i;
	int *playlist;
	
	playlist = (int*) calloc( songs, sizeof(int) );
	for (i=0; i<songs; i++) {
		playlist[i] = i;
	}

	/* reinit RNG with current seed - playlists shall be reproducible! */
	srand(random_seed);
	shuffle_int(playlist, songs);

	return playlist;
}

static regparm int get_next_subsong(struct gbs *gbs)
/* returns the number of the subsong that is to be played next */
{
	int next = -1;
	switch (playmode) {

	case PLAYMODE_RANDOM:
		next = rand_int(gbs->songs);
		break;

	case PLAYMODE_SHUFFLE:
		subsong_playlist_idx++;
		if (subsong_playlist_idx == gbs->songs) {
			free(subsong_playlist);
			random_seed++;
			subsong_playlist = setup_playlist(gbs->songs);
			subsong_playlist_idx = 0;
		}
		next = subsong_playlist[subsong_playlist_idx];
		break;

	case PLAYMODE_LINEAR:
		next = gbs->subsong + 1;
		break;
	}
	return next;
}

static regparm int get_prev_subsong(struct gbs *gbs)
/* returns the number of the subsong that has been played previously */
{
	int prev = -1;
	switch (playmode) {

	case PLAYMODE_RANDOM:
		prev = rand_int(gbs->songs);
		break;

	case PLAYMODE_SHUFFLE:
		subsong_playlist_idx--;
		if (subsong_playlist_idx == -1) {
			free(subsong_playlist);
			random_seed--;
			subsong_playlist = setup_playlist(gbs->songs);
			subsong_playlist_idx = gbs->songs-1;
		}
		prev = subsong_playlist[subsong_playlist_idx];
		break;

	case PLAYMODE_LINEAR:
		prev = gbs->subsong - 1;
		break;
	}
	return prev;
}

static regparm void setup_playmode(struct gbs *gbs)
/* initializes the chosen playmode (set start subsong etc.) */
{
	switch (playmode) {

	case PLAYMODE_RANDOM:
		if (gbs->subsong == -1) {
			gbs->subsong = get_next_subsong(gbs);
		}

	case PLAYMODE_SHUFFLE:
		subsong_playlist = setup_playlist(gbs->songs);
		subsong_playlist_idx = 0;
		if (gbs->subsong == -1) {
			gbs->subsong = subsong_playlist[0];
		} else {
			/* randomize playlist until desired start song is first */
			/* (rotation does not work because this must be reproducible */
			/* by setting random_seed to the old value */
			while (subsong_playlist[0] != gbs->subsong) {
				random_seed++;
				subsong_playlist = setup_playlist(gbs->songs);
			}
		}

	case PLAYMODE_LINEAR:
		if (gbs->subsong == -1) {
			gbs->subsong = gbs->defaultsong - 1;
		}
		break;
	}
}

static regparm int nextsubsong_cb(struct gbs *gbs, void *priv)
{
	int subsong = get_next_subsong(gbs);

	if (gbs->subsong == subsong_stop ||
	    subsong >= gbs->songs)
		return false;

	gbs_init(gbs, subsong);
	return true;
}

static regparm void select_plugin_by_index(int idx)
{
	sound_id    = plugouts[idx].id;
	sound_name  = plugouts[idx].name;
	sound_open  = plugouts[idx].open_fn;
	sound_write = plugouts[idx].write_fn;
	sound_close = plugouts[idx].close_fn;
}

static regparm void select_plugin(void)
{
	/* autoselect:  (make this more intelligent) */
	select_plugin_by_index(0);
}

static regparm void select_plugin_by_id(char *id)
{
	int idx;

	if (strcmp(id, "list") == 0) {
		printf(_("available output plugins:\n"));
		for (idx = 0; plugouts[idx].write_fn != NULL; idx++) {
			printf("%s\t- %s\n", plugouts[idx].id, plugouts[idx].name);
		}
		exit(0);
	}

	for (idx = 0; plugouts[idx].write_fn != NULL; idx++) {
		if ( strcmp(plugouts[idx].id, id) == 0 ) {
			select_plugin_by_index(idx);
			return;
		}
	}
	
	printf(_("\"%s\" is not a known output plugin.\n\n"), id);
	exit(1);
}

char *endian_str(int endian)
{
	switch (endian) {
	case CFG_ENDIAN_BE: return "big";
	case CFG_ENDIAN_LE: return "little";
	case CFG_ENDIAN_NE: return "native";
	default: return "invalid";
	}
}

static regparm void usage(int exitcode)
{
	FILE *out = exitcode ? stderr : stdout;
	fprintf(out,
	        _("Usage: %s [option(s)] <gbs-file> [start_at_subsong [stop_at_subsong] ]\n"
	        "\n"
	        "Available options are:\n"
	        "  -E  endian, b == big, l == little, n == native (%s)\n"
	        "  -f  set fadeout (%d seconds)\n"
	        "  -g  set subsong gap (%d seconds)\n"
	        "  -h  display this help and exit\n"
	        "  -o  select output plugin (%s)\n"
	        "      'list' shows available plugins\n"
	        "  -q  quiet\n"
	        "  -r  set samplerate (%dHz)\n"
	        "  -R  set refresh delay (%d milliseconds)\n"
	        "  -t  set subsong timeout (%d seconds)\n"
	        "  -T  set silence timeout (%d seconds)\n"
	        "  -V  print version and exit\n"
		"  -z  play subsongs in shuffle mode\n"
		"  -Z  play subsongs in random mode (repetitions possible)\n"),
	        myname,
	        endian_str(endian),
		fadeout,
		subsong_gap,
		sound_id,
	        rate,
		refresh_delay,
		subsong_timeout,
	        silence_timeout);
	exit(exitcode);
}

static regparm void version(void)
{
	puts("gbsplay " GBS_VERSION);
	exit(0);
}

static regparm void parseopts(int *argc, char ***argv)
{
	int res;
	myname = *argv[0];
	while ((res = getopt(*argc, *argv, "E:f:g:hqo:r:R:t:T:VzZ")) != -1) {
		switch (res) {
		default:
			usage(1);
			break;
		case 'E':
			if (strcasecmp(optarg, "b") == 0) {
				endian = CFG_ENDIAN_BE;
			} else if (strcasecmp(optarg, "l") == 0) {
				endian = CFG_ENDIAN_LE;
			} else if (strcasecmp(optarg, "n") == 0) {
				endian = CFG_ENDIAN_NE;
			} else {
				printf(_("\"%s\" is not a valid endian.\n\n"), optarg);
				usage(1);
			}
			break;
		case 'f':
			sscanf(optarg, "%d", &fadeout);
			break;
		case 'g':
			sscanf(optarg, "%d", &subsong_gap);
			break;
		case 'h':
			usage(0);
			break;
		case 'o':
			select_plugin_by_id(optarg);
			break;
		case 'q':
			quiet = 1;
			break;
		case 'r':
			sscanf(optarg, "%d", &rate);
			break;
		case 'R':
			sscanf(optarg, "%d", &refresh_delay);
			break;
		case 't':
			sscanf(optarg, "%d", &subsong_timeout);
			break;
		case 'T':
			sscanf(optarg, "%d", &silence_timeout);
			break;
		case 'V':
			version();
			break;
		case 'z':
			playmode = PLAYMODE_SHUFFLE;
			break;
		case 'Z':
			playmode = PLAYMODE_RANDOM;
			break;
		}
	}
	*argc -= optind;
	*argv += optind;
}

static regparm void handleuserinput(struct gbs *gbs)
{
	char c;

	if (read(STDIN_FILENO, &c, 1) != -1) {
		switch (c) {
		case 'p':
			gbs->subsong = get_prev_subsong(gbs);
			while (gbs->subsong < 0) {
				gbs->subsong += gbs->songs;
			}
			gbs_init(gbs, gbs->subsong);
			break;
		case 'n':
			gbs->subsong = get_next_subsong(gbs);
			gbs->subsong %= gbs->songs;
			gbs_init(gbs, gbs->subsong);
			break;
		case 'q':
		case 27:
			quit = 1;
			break;
		case ' ':
			pause_mode = !pause_mode;
			gbhw_pause(pause_mode);
			break;
		case '1':
		case '2':
		case '3':
		case '4':
			gbhw_ch[c-'1'].mute ^= 1;
			break;
		}
	}
}

static regparm char *notestring(int ch)
{
	int n;

	if (gbhw_ch[ch].mute) return "-M-";

	if (gbhw_ch[ch].volume == 0 ||
	    gbhw_ch[ch].master == 0) return "---";

	n = getnote(gbhw_ch[ch].div_tc);
	if (ch != 3) return &notelookup[4*n];
	else return "nse";
}

static regparm int chvol(int ch)
{
	int v;

	if (gbhw_ch[ch].mute ||
	    gbhw_ch[ch].master == 0) return 0;

	if (ch == 2)
		v = (3-((gbhw_ch[2].volume+3)&3)) << 2;
	else v = gbhw_ch[ch].volume;

	return v;
}

static regparm char *volstring(int v)
{
	if (v < 0) v = 0;
	if (v > 15) v = 15;

	return &vollookup[5*v];
}

static regparm void printstatus(struct gbs *gbs)
{
	int time = gbs->ticks / GBHW_CLOCK;
	int timem = time / 60;
	int times = time % 60;
	char *songtitle;
	int len = gbs->subsong_info[gbs->subsong].len / 1024;
	int lenm, lens;

	if (len == 0) {
		len = subsong_timeout;
	}
	lenm = len / 60;
	lens = len % 60;

	songtitle = gbs->subsong_info[gbs->subsong].title;
	if (!songtitle) {
		songtitle=_("Untitled");
	}
	printf("\r\033[A\033[A"
	       "Song %3d/%3d (%s)\033[K\n"
	       "%02d:%02d/%02d:%02d  %s %s  %s %s  %s %s  %s %s  [%s|%s]\n",
	       gbs->subsong+1, gbs->songs, songtitle,
	       timem, times, lenm, lens,
	       notestring(0), volstring(chvol(0)),
	       notestring(1), volstring(chvol(1)),
	       notestring(2), volstring(chvol(2)),
	       notestring(3), volstring(chvol(3)),
	       reverse_vol(volstring(gbs->lvol/1024)),
	       volstring(gbs->rvol/1024));
	fflush(stdout);
}

/*
 * signal handlers and main may not use regparm
 * unless libc is using regparm too...
 */
void exit_handler(int signum)
{
	printf(_("\nCaught signal %d, exiting...\n"), signum);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &ots);
	exit(1);
}

void stop_handler(int signum)
{
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &ots);
}

void cont_handler(int signum)
{
	struct termios ts;

	tcgetattr(STDIN_FILENO, &ts);
	ots = ts;
	ts.c_lflag &= ~(ICANON | ECHO | ECHONL);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &ts);
	fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);

	redraw = true;
}

static regparm void printinfo(struct gbs *gbs)
{
	if (!quiet) {
		gbs_printinfo(gbs, 0);
		puts(_("\ncommands:  [p]revious subsong   [n]ext subsong   [q]uit player\n" \
		         "           [ ] pause/resume   [1-4] mute channel"));
		puts("\n\n"); /* additional newlines for the status display */
	}
	redraw = false;
}

int main(int argc, char **argv)
{
	struct gbs *gbs;
	char *homedir = getenv("HOME");
	char *usercfg = malloc(strlen(homedir) + strlen(cfgfile) + 2);
	struct termios ts;
	struct sigaction sa;
	int subsong = -1;

	i18n_init();

	/* initialize RNG */
	random_seed = time(0)+getpid();
	srand(random_seed);

	select_plugin();

	sprintf(usercfg, "%s/%s", homedir, cfgfile);
	cfg_parse("/etc/gbsplayrc", options);
	cfg_parse(usercfg, options);
	parseopts(&argc, &argv);

	if (argc < 1) {
		usage(1);
	}

	precalc_notes();
	precalc_vols();

	sound_open(endian, rate);

	gbhw_setcallback(callback, NULL);
	gbhw_setrate(rate);

	if (argc >= 2) {
		sscanf(argv[1], "%d", &subsong);
		subsong--;
	}

	if (argc >= 3) {
		sscanf(argv[2], "%d", &subsong_stop);
		subsong_stop--;
	}

	gbs = gbs_open(argv[0]);
	if (gbs == NULL) {
		exit(1);
	}

	/* sanitize commandline values */
	if (subsong < -1) {
		subsong = 0;
	} else if (subsong >= gbs->songs) {
		subsong = gbs->songs-1;
	}
	if (subsong_stop <  0) {
		subsong_stop = -1;
	} else if (subsong_stop >= gbs->songs) {
		subsong_stop = -1;
	}
	
	gbs->subsong = subsong;
	gbs->subsong_timeout = subsong_timeout;
	gbs->silence_timeout = silence_timeout;
	gbs->gap = subsong_gap;
	gbs->fadeout = fadeout;
	setup_playmode(gbs);
	gbhw_setbuffer(&buf);
	gbs_set_nextsubsong_cb(gbs, nextsubsong_cb, NULL);
	gbs_init(gbs, gbs->subsong);
	printinfo(gbs);
	tcgetattr(STDIN_FILENO, &ts);
	ots = ts;
	ts.c_lflag &= ~(ICANON | ECHO | ECHONL);
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &ts);

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = exit_handler;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);
	sa.sa_handler = stop_handler;
	sigaction(SIGSTOP, &sa, NULL);
	sa.sa_handler = cont_handler;
	sigaction(SIGCONT, &sa, NULL);

	fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
	while (!quit) {
		if (!gbs_step(gbs, refresh_delay)) {
			quit = 1;
			break;
		}

		if (redraw) printinfo(gbs);
		if (!quiet) printstatus(gbs);
		handleuserinput(gbs);
	}
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &ots);

	sound_close();

	return 0;
}
