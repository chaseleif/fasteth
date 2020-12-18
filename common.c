#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <arpa/inet.h>
#include <netinet/tcp.h> //TCP_NODELAY

// inserts the int x in the first 4 bytes
void intinbuffer(unsigned char *buffer,const int x) {
	for (int shmt=24,i=0;shmt>=0;++i,shmt-=8) {
		buffer[i] = (x>>shmt)&0xFF;
	}
}

// inserts the ull x in the first 8 bytes
void ullinbuffer(unsigned char *buffer,const unsigned long long x) {
	for (int shmt=56,i=0;shmt>=0;++i,shmt-=8) {
		buffer[i] = (x>>shmt)&0xFF;
	}
}

// returns the int from the first 4 bytes
int intfrombuffer(unsigned char *buffer) {
	int ret=0;
	for (int shmt=24,i=0;shmt>=0;++i,shmt-=8) {
		ret|= ((int)buffer[i])<<shmt;
	}
	return ret;
}

// returns the ull from the first 8 bytes
unsigned long long ullfrombuffer(unsigned char *buffer) {
	unsigned long long ret=0;
	for (int shmt=56,i=0;shmt>=0;++i,shmt-=8) {
		ret|= ((unsigned long long)buffer[i])<<shmt;
	}
	return ret;
}

// sends buffer, of length size, to socket fd
// returns 0 for failure, 1 for success
// does not give up if the socket would block or try again
unsigned char sendbuffer(int fd,void *buffer,int length) {
//TCP_QUICKACK // not for portable code. immediately sends ack, must be continually re-set
//	setsockopt(fd,IPPROTO_TCP,TCP_CORK,(const void*)&optval,sizeof(int));
//	int optval=1;
//	setsockopt(fd,IPPROTO_TCP,TCP_QUICKACK,(const void*)&optval,sizeof(int));
//	setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,(const void*)&optval,sizeof(int)); //disable Nagle algorithm
	int optval=16;
	setsockopt(fd,SOL_SOCKET,SO_SNDLOWAT,(const void*)&optval,sizeof(int));
	int i=0;
	while (i<length) {
		int ret = write(fd,buffer+i,length-i);
		if (ret<1) {
			if (errno==EWOULDBLOCK || errno==EAGAIN) {
				sleep(1);
				continue;
			}
			return 0;
		}
		i+=ret;
	}
	return 1;
}

// receives buffer, of length size, from socket fd
// returns 0 for failure, 1 for success
unsigned char rcvbuffer(int fd,void *buffer,int length) {
	int optval=16;
	setsockopt(fd,SOL_SOCKET,SO_RCVLOWAT,(const void*)&optval,sizeof(int));
	int i=0;
	while (i<length) {
		int ret = read(fd,buffer+i,length-i);
		if (ret<1) {
			if (ret && (errno==EWOULDBLOCK || errno==EAGAIN)) {
				sleep(1);
				continue;
			}
			return 0;
		}
		i+=ret;
	}
	return 1;
}

unsigned char semiblockrcv(int fd,void *buffer,int length) {
	int optval=16;
	setsockopt(fd,SOL_SOCKET,SO_RCVLOWAT,(const void*)&optval,sizeof(int));
	int i=0;
	while (i<length) {
		int ret = read(fd,buffer+i,length-i);
		if (ret<1) return 0;
		i+=ret;
	}
	return 1;
}
