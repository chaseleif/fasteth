#ifndef _FASTETH_COMMON_H
#define _FASTETH_COMMON_H

// inserts the int x in the first 4 bytes
void intinbuffer(unsigned char *buffer,const int x);
// inserts the ull x in the first 8 bytes
void ullinbuffer(unsigned char *buffer,const unsigned long long x);
// returns the int from the first 4 bytes
int intfrombuffer(unsigned char *buffer);
// returns the ull from the first 8 bytes
unsigned long long ullfrombuffer(unsigned char *buffer);

// sends buffer, of length size, to socket fd
// returns 0 for failure, 1 for success
unsigned char sendbuffer(int fd,void *buffer,int length);

// receives buffer, of length size, from socket fd
// returns 0 for failure, 1 for success
unsigned char rcvbuffer(int fd,void *buffer,int length);

// attempts to receive buffer, this doesn't check for EAGAIN
unsigned char semiblockrcv(int fd,void *buffer,int length);


#endif // _FASTETH_COMMON_H
