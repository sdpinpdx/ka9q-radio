// $Id: sdr.h,v 1.4 2017/05/20 01:18:04 karn Exp karn $
#ifndef _SDR_H
#define _SDR_H 1

#include <stdint.h>
#include <complex.h>
#undef I

int mirics_gain(double f,int gr,uint8_t *bb, uint8_t *lna,uint8_t *mix);
int front_end_init(int,int,int);
int get_adc(short *,int);
void closedown(int);

#endif
