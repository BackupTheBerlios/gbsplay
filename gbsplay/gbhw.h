/* $Id: gbhw.h,v 1.6 2003/10/11 18:25:25 ranma Exp $
 *
 * gbsplay is a Gameboy sound player
 *
 * 2003 (C) by Tobias Diedrich <ranma@gmx.at>
 * Licensed under GNU GPL.
 */

#ifndef _GBHW_H_
#define _GBHW_H_

struct gbhw_channel {
	int master;
	int leftgate;
	int rightgate;
	int volume;
	int env_dir;
	int env_tc;
	int env_ctr;
	int sweep_dir;
	int sweep_tc;
	int sweep_ctr;
	int sweep_shift;
	int len;
	int len_enable;
	int div_tc;
	int div_ctr;
	int duty_tc;
	int duty_ctr;
};

extern struct gbhw_channel gbhw_ch[4];
extern int master_volume;
extern int master_fade;

typedef void (*gbhw_callback_fn)(void *buf, int len, void *priv);

void gbhw_setcallback(gbhw_callback_fn fn, void *priv);
void gbhw_setrate(int rate);
void gbhw_init(unsigned char *rombuf, unsigned int size);
void gbhw_pause(int new_pause);
int gbhw_step(int time_to_work);

#endif
