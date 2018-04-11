#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#if defined(linux)
#include <bsd/string.h>
#endif
#include "multicast.h"

int Mcast_ttl = 1;


static void soptions(int fd){
  // Failures here are not fatal
  int reuse = 1;
  if(setsockopt(fd,SOL_SOCKET,SO_REUSEPORT,&reuse,sizeof(reuse)) != 0)
    perror("so_reuseport failed");
  if(setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse)) != 0)
    perror("so_reuseaddr failed");
  struct linger linger;
  linger.l_onoff = 0;
  linger.l_linger = 0;
  if(setsockopt(fd,SOL_SOCKET,SO_LINGER,&linger,sizeof(linger)) != 0)
    perror("so_linger failed");
  u_char ttl = Mcast_ttl;
  if(setsockopt(fd,IPPROTO_IP,IP_MULTICAST_TTL,&ttl,sizeof(ttl)) != 0){
    perror("so_ttl failed");
  }
  u_char loop = 1;
  if(setsockopt(fd,IPPROTO_IP,IP_MULTICAST_LOOP,&loop,sizeof(loop)) != 0){
    perror("so_ttl failed");
  }
}

#if defined(linux) // Linux, etc, for both IPv4/IPv6
static int join_group(int fd,struct addrinfo *resp){
  struct sockaddr_in const *sin = (struct sockaddr_in *)resp->ai_addr;
  if(!IN_MULTICAST(ntohl(sin->sin_addr.s_addr)))
    return -1;

  struct group_req group_req;
  group_req.gr_interface = 0;
  memcpy(&group_req.gr_group,resp->ai_addr,resp->ai_addrlen);
  if(setsockopt(fd,IPPROTO_IP,MCAST_JOIN_GROUP,&group_req,sizeof(group_req)) != 0){
    perror("multicast join");
    return -1;
  }
  return 0;
}
#else // old version, seems required on Apple    
static int join_group(int fd,struct addrinfo *resp){
  struct sockaddr_in const *sin = (struct sockaddr_in *)resp->ai_addr;
  if(!IN_MULTICAST(ntohl(sin->sin_addr.s_addr)))
     return -1;

  struct ip_mreq mreq;
  mreq.imr_multiaddr = sin->sin_addr;
  mreq.imr_interface.s_addr = INADDR_ANY;
  if(setsockopt(fd,IPPROTO_IP,IP_ADD_MEMBERSHIP,&mreq,sizeof(mreq)) != 0){
    perror("multicast join");
    return -1;
  }
  return 0;
}
#endif

char Default_mcast_port[] = "5004";

// Set up multicast socket for input or output
// Target is in the form of domain.name.com:5004 or 1.2.3.4:5004
int setup_mcast(char const *target,int output){
  int len = strlen(target) + 1;  // Including terminal null
  char host[len],*port;

  strlcpy(host,target,len);
  if((port = strrchr(host,':')) != NULL){
    *port++ = '\0';
  } else {
    port = Default_mcast_port; // Default for RTP
  }

  struct addrinfo hints;
  memset(&hints,0,sizeof(hints));
  hints.ai_family = AF_INET; // Only IPv4 for now (grrr....)
  hints.ai_socktype = SOCK_DGRAM;
  hints.ai_flags = AI_NUMERICSERV;

  struct addrinfo *results = NULL;
  int ecode;
  if((ecode = getaddrinfo(host,port,&hints,&results)) != 0){
    fprintf(stderr,"setup_mcast getaddrinfo(%s,%s): %s\n",host,port,gai_strerror(ecode));
    return -1;
  }
  struct addrinfo *resp;
  int fd = -1;
  for(resp = results; resp != NULL; resp = resp->ai_next){
    if((fd = socket(resp->ai_family,resp->ai_socktype,resp->ai_protocol)) < 0)
      continue;
    soptions(fd);
    if(output){
      if((connect(fd,resp->ai_addr,resp->ai_addrlen) == 0))
	break;
    } else { // input
      if((bind(fd,resp->ai_addr,resp->ai_addrlen) == 0))
	break;
    }
    close(fd);
    fd = -1;
  }
  // Strictly speaking, it is not necessary to join a multicast group to which we only send.
  // But this creates a problem with brain-dead Netgear (and probably other) "smart" switches
  // that do IGMP snooping. There's a setting to handle what happens with multicast groups
  // to which no IGMP messages are seen. If set to discard them, IPv6 multicast breaks
  // because there's no IPv6 multicast querier. But set to pass them, then IPv4 multicasts
  // that aren't subscribed to by anybody are flooded everywhere! We avoid that by subscribing
  // to our own multicasts.

  if(fd != -1)
    join_group(fd,resp);
  else
    fprintf(stderr,"setup_input: Can't create multicast socket for %s:%s\n",host,port);

#if 0 // testing hack - find out if we're using source specific multicast (we're not)
  {
  struct in_addr interface,group;
  uint32_t fmode;
  uint32_t numsrc;
  struct in_addr slist[100];
  int n;

  struct sockaddr_in const *sin = (struct sockaddr_in *)resp->ai_addr;

  
  interface.s_addr = htonl(0xc0a82c07);
  group = sin->sin_addr;
  fmode = MCAST_INCLUDE;
  numsrc = 100;
  printf("fd = %d\n",fd);

  n = getipv4sourcefilter(fd,interface,group,&fmode,&numsrc,slist);
  if(n < 0)
    perror("getipv4sourcefilter");
  printf("n = %d numsrc = %d\n",n,numsrc);
  }
#endif


  freeaddrinfo(results);
  return fd;
}

unsigned char *ntoh_rtp(struct rtp_header *rtp,unsigned char *data){
  rtp->version = data[0] >> 6;
  rtp->pad = (data[0] >> 5) & 1;
  rtp->extension = (data[0] >> 4) & 1;
  rtp->cc = data[0] & 0xf;
  rtp->marker = (data[1] >> 7) & 1;
  rtp->type = data[1] & 0x7f;
  rtp->seq = (data[2] << 8) | data[3];
  rtp->timestamp = data[4] << 24 | data[5] << 16 | data[6] << 8 | data[7];
  rtp->ssrc = data[8] << 24 | data[9] << 16 | data[10] << 8 | data[11];
  for(int i=0; i<rtp->cc; i++)
    rtp->csrc[i] = data[12+4*i] << 24 | data[13+4*i] << 16 | data[14+4*i] << 8 | data[15+4*i];
  
  return data + RTP_MIN_SIZE + rtp->cc * sizeof(uint32_t);
}


unsigned char *hton_rtp(unsigned char *data, struct rtp_header *rtp){
  rtp->cc &= 0xf; // Force it to be legal
  rtp->type &= 0x7f;
  data[0] = (rtp->version << 6) | (rtp->pad << 5) | (rtp->extension << 4) | rtp->cc;
  data[1] = (rtp->marker << 7) | rtp->type;
  data[2] = rtp->seq >> 8;
  data[3] = rtp->seq;

  data[4] = rtp->timestamp >> 24;
  data[5] = rtp->timestamp >> 16;
  data[6] = rtp->timestamp >> 8;
  data[7] = rtp->timestamp;

  data[8] = rtp->ssrc >> 24;
  data[9] = rtp->ssrc >> 16;
  data[10] = rtp->ssrc >> 8;
  data[11] = rtp->ssrc;

  for(int i=0; i < rtp->cc; i++){
    data[12+4*i] = rtp->csrc[i] >> 24;
    data[13+4*i] = rtp->csrc[i] >> 16;
    data[14+4*i] = rtp->csrc[i] >> 8;
    data[15+4*i] = rtp->csrc[i];
  }
  
  return data + RTP_MIN_SIZE + rtp->cc * sizeof(uint32_t);
}
