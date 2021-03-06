// $Id: metadump.c,v 1.2 2018/12/12 08:38:42 karn Exp karn $
// Utility to trace multicast SDR metadata
// Copyright 2018 Phil Karn, KA9Q

#define _GNU_SOURCE 1
#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <pthread.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include <math.h>
#include <complex.h>
#undef I
#include <sys/time.h>
#include <ncurses.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netdb.h>
#include <locale.h>

#include "misc.h"
#include "multicast.h"
#include "status.h"

int Status_sock;

int Verbose,Dump;

char Locale[256] = "en_US.UTF-8";

int main(int argc,char *argv[]){
  int c;

  while((c = getopt(argc,argv,"vd")) != EOF){
    switch(c){
    case 'v':
      Verbose++;
      break;
    }
  }

  {
    char const * const cp = getenv("LANG");
    if(cp != NULL){
      strlcpy(Locale,cp,sizeof(Locale));
    }
  }
  setlocale(LC_ALL,Locale); // Set either the hardwired default or the value of $LANG if it exists
  fprintf(stderr,"Listening to %s\n",argv[optind]);
  Status_sock = setup_mcast(argv[optind],NULL,0,0,2);

  struct sockcache sc;
  for(;;){
    unsigned char buffer[8192];

    memset(buffer,0,sizeof(buffer));
    struct sockaddr_storage source;
    socklen_t len = sizeof(source);
    int length = recvfrom(Status_sock,buffer,sizeof(buffer),0,(struct sockaddr *)&source,&len);
    if(length <= 0){
      sleep(1);
      continue;
    }
    update_sockcache(&sc,(struct sockaddr *)&source);
    int cr = buffer[0]; // Command/response byte
    printf("%s:%s: %s",sc.host,sc.port,cr ? "CMD " : "STAT");
    dump_metadata(buffer+1,length-1);
  }
}


