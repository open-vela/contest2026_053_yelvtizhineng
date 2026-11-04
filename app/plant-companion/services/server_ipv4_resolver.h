#ifndef SERVER_IPV4_RESOLVER_H
#define SERVER_IPV4_RESOLVER_H
#include <arpa/inet.h>
#include <netdb.h>
#include <errno.h>
#include <string.h>
/* Every bridge socket is AF_INET. Use a private result, not the shared
 * gethostbyname hostent, and never copy an IPv6 address into in_addr. */
static inline int sb_resolve_ipv4(const char *host,struct in_addr *address)
{
  if(!host || !address)return -EINVAL;
  struct in_addr literal;
  if(inet_pton(AF_INET,host,&literal)==1){*address=literal;return 0;}
  struct addrinfo hints,*result=NULL;
  memset(&hints,0,sizeof(hints));hints.ai_family=AF_INET;hints.ai_socktype=SOCK_STREAM;
  if(getaddrinfo(host,NULL,&hints,&result)!=0)return -ENOENT;
  int status=-EAFNOSUPPORT;
  for(struct addrinfo *it=result;it;it=it->ai_next){
    if(it->ai_family==AF_INET && it->ai_addr &&
       it->ai_addrlen>=sizeof(struct sockaddr_in) && it->ai_addr->sa_family==AF_INET){
      *address=((struct sockaddr_in *)it->ai_addr)->sin_addr;status=0;break;
    }
  }
  if(result)freeaddrinfo(result);
  return status;
}
#endif
