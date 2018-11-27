// $Id: modes.c,v 1.27 2018/11/25 02:59:04 karn Exp karn $
// Load and search mode definition table in /usr/local/share/ka9q-radio/modes.txt

// Copyright 2018, Phil Karn, KA9Q
#include <assert.h>
#include <errno.h>
#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <string.h>


#include "misc.h"
#include "radio.h"



#define MAXMODES 256
struct modetab Modes[MAXMODES];
int Nmodes;

extern char Libdir[];

extern struct demodtab Demodtab[];
extern int Ndemod;

int readmodes(char *file){
  char pathname[PATH_MAX];
  snprintf(pathname,sizeof(pathname),"%s/%s",Libdir,file);
  FILE * const fp = fopen(pathname,"r");
  if(fp == NULL){
    fprintf(stderr,"Can't read mode table %s:%s\n",pathname,strerror(errno));
    return -1;
  }
  char line[PATH_MAX];
  while(fgets(line,sizeof(line),fp) != NULL){
    chomp(line);
    // Everything after # is a comment
    char *cp = strchr(line,'#');
    if(cp != NULL)
      *cp = '\0';

    struct modetab *mtp = &Modes[Nmodes];

    // Parse line (C is pretty weak on text processing...)

    char *stringp = line;

    char *mode_name;
    do {
      mode_name = strsep(&stringp," \t");
    } while (mode_name != NULL && *mode_name == '\0');
    
    char *demod_name;
    do {
      demod_name = strsep(&stringp," \t");
    } while (demod_name != NULL && *demod_name == '\0');
    
    if(mode_name == NULL || demod_name == NULL)
      continue;
    
    struct demodtab *dtp;
    for(dtp = &Demodtab[0]; dtp < &Demodtab[Ndemod]; dtp++){
      if(strncasecmp(demod_name,dtp->name,strlen(dtp->name)) == 0)
	break;
    }
    if(dtp == &Demodtab[Ndemod])
      continue; // Demod not found in list

    mtp->demod_index = dtp - &Demodtab[0];
    strlcpy(mtp->name, mode_name, sizeof(mtp->name));

    double low,high;
    low = strtod(stringp,&stringp);
    high = strtod(stringp,&stringp);
    if(high < low){ // Ensure high > low
      mtp->low = high;
      mtp->high = low;
    } else {
      mtp->low = low;
      mtp->high = high;
    }
    mtp->shift = strtod(stringp,&stringp);
    mtp->attack_rate = -fabs(strtod(stringp,&stringp));
    mtp->recovery_rate = fabs(strtod(stringp,&stringp));
    mtp->hangtime = fabs(strtod(stringp,&stringp)); // Must be positive

    // Process options
    mtp->flags = 0;
    for(int i=0;i<8;i++){
      char *option;
      // Skip leading space
      do {
	option = strsep(&stringp," \t");
      } while (option != NULL && *option == '\0');
      if(option == NULL)
	break; // No more

      if(strcasecmp(option,"isb") == 0 || strcasecmp(option,"conj") == 0){
	mtp->flags |= ISB;         // For independent sideband: LSB on left, USB on right
      } else if(strcasecmp(option,"flat") == 0){
	mtp->flags |= FLAT;         // FM only
      } else if(strcasecmp(option,"square") == 0){
	mtp->flags |= SQUARE|PLL; // Square implies PLL
      } else if(strcasecmp(option,"coherent") == 0 || strcasecmp(option,"pll") == 0){
	mtp->flags |= PLL;
      } else if(strcasecmp(option,"mono") == 0){
	mtp->flags |= MONO;  // E.g., if you don't want the hilbert transform of SSB on the right channel
      }
    }    
    Nmodes++;
    if(Nmodes == MAXMODES)
      break; // table full
  }
  return 0;
}

