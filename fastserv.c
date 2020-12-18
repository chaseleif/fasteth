#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <netinet/tcp.h> //TCP_NODELAY
#include <fcntl.h>
#include <sys/select.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include "common.h"

// static size at front of every packet
// every packet begins with 4bytes=src, 4bytes=dst
// the last 8 bytes are either total transfer size (for initial data request)
// or they are two integers, the first is the sequence number of the current transfer
// the second is the remaining data size of the current packet (up to MAXDATASIZE)
#define INITFRAMESIZE 16
// the max frame size, max size for send calls
#define MAXFRAMESIZE 4096
// max amount of data in a data packet, is the frame size minus the init size
#define MAXDATASIZE MAXFRAMESIZE-INITFRAMESIZE

// queuesizes MUST be >= 1
// this does work with 64 processes and both queues with size 1
// 
#define REQUESTQUEUESIZE 10
#define DATAQUEUESIZE 2

// we have an array of these -> dataqueue[DATAQUEUESIZE]
// src_sp_id = -1 to indicate unoccupied
// has its own buffer, so whoever reserved an index will have a buffer of their own
// also holds the dst_sp_id and the bytes remaining (actual filesize bytes)
typedef struct dataqueuenode {
	unsigned char buffer[MAXFRAMESIZE];
	unsigned long long bytesremaining;
	int src_sp_id;
	int dst_sp_id;
}dataqueuenode;

// we have an array of these -> requestqueue[REQUESTQUEUESIZE]
// src_sp_id = -1 to indicate unoccupied
// also holds the dst_sp_id and the total size of the pending transfer (actual filesize bytes)
typedef struct requestqueuenode {
	int src_sp_id;
	int dst_sp_id;
	unsigned long long datasize;
}requestqueuenode;

// Utility functions, beginning with queue helper functions:

// gives the next index of the data queue that doesn't have a source sp id set
// if the data queue is not full this returns an index
// if the data queue is full this returns -1
static inline int getnextdataqindex(dataqueuenode *queue) {
	for (int i=0;i<DATAQUEUESIZE;++i) {
		if (queue[i].src_sp_id<0) {
			return i;
		}
	}
	return -1;
}

// add a request to the queue, takes the queue array and all details of the transaction
// attempts to add the request to the end of the array
// if the request is added returns 1
// if the queue is full returns 0
static inline unsigned char queuerequest(requestqueuenode *queue,const int src_sp_id,const int dst_sp_id,const unsigned long long reqsize) {
	if (queue[0].src_sp_id<0) {
		queue[0].src_sp_id=src_sp_id;
		queue[0].dst_sp_id=dst_sp_id;
		queue[0].datasize=reqsize;
		return 1;
	}
	else {
		for (int i=0;i<REQUESTQUEUESIZE;++i) {
			if (queue[i].src_sp_id<0) {
				queue[i].src_sp_id=src_sp_id;
				queue[i].dst_sp_id=dst_sp_id;
				queue[i].datasize=reqsize;
				return 1;
			}
		}
	}
	return 0;
}

// gets the next request from the queue, pops from the front and fills out the *result parameter
// shifts later elements forward, sets the final element's src_sp_id to -1 (to handle if the queue was full)
// the return value is put in the result parameter, ***set its sp_id to -1 before calling this function***
// if the sp_id is set all the member vars are also set, otherwise nothing was in the queue
static inline void getrequest(requestqueuenode *queue,requestqueuenode *result) {
	if (queue[0].src_sp_id<0) return;
	result->src_sp_id = queue[0].src_sp_id;
	result->dst_sp_id = queue[0].dst_sp_id;
	result->datasize = queue[0].datasize;
	for (int i=1;i<REQUESTQUEUESIZE;++i) {
		queue[i-1].src_sp_id=queue[i].src_sp_id;
		queue[i-1].dst_sp_id=queue[i].dst_sp_id;
		queue[i-1].datasize=queue[i].datasize;
		if (queue[i].src_sp_id<0) return; // rest are -1 already
	}
	queue[REQUESTQUEUESIZE-1].src_sp_id=-1;
}

// shifts an element from the request queue to the data queue
// if successful, returns the index to the data queue element for notification
// if no shift can be made returns -1
static int shiftqueues(requestqueuenode *requestqueue,dataqueuenode *dataqueue,int *sp) {
	// try to move something from the request queue to the data queue
	if (requestqueue[0].src_sp_id>=0 && requestqueue[0].dst_sp_id>=0 && getnextdataqindex(dataqueue)>=0) {
		// either the destination SP is invalid or has not connected yet
		if (sp[requestqueue[0].dst_sp_id]<0) return -1; // don't cause problems, let's just come back for this
		// we have an open spot in the data queue
		requestqueuenode result = { .src_sp_id=-1 };
		getrequest(requestqueue,&result);
		// we retrieved a request from the queue
		if (result.src_sp_id>=0) {
			// put the request queue into the data queue
			const int dataqindex = getnextdataqindex(dataqueue);
			dataqueue[dataqindex].src_sp_id = result.src_sp_id;
			dataqueue[dataqindex].dst_sp_id = result.dst_sp_id;
			dataqueue[dataqindex].bytesremaining = result.datasize;
			return dataqindex;
		}
	}
	return -1;
}

// takes a port number
// returns a listening socket for the CSP
// TCP non-blocking socket
static int getlisteningsocket(unsigned short portnum) {
	struct sockaddr_in addr;
	memset((void*)&addr,0,sizeof(struct sockaddr_in));
	addr.sin_port=htons(portnum);
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	int fd =socket(AF_INET,SOCK_STREAM | SOCK_NONBLOCK,IPPROTO_TCP);
	if (fd<0) {
		fprintf(stderr,"CSP: Error getting a new socket\n");
		return -1;
	}
	int optval=1;
	setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,(const void*)&optval,sizeof(int));
	setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,(const void*)&optval,sizeof(int));
	if (bind(fd,(struct sockaddr*)&addr,sizeof(addr))<0) {
		fprintf(stderr,"CSP: Unable to bind socket\n");
		close(fd);
		return -1;
	}
	if ((listen(fd,1))<0) {
		fprintf(stderr,"CSP: Unable to listen to socket\n");
		close(fd);
		return -1;
	}
	return fd;
}

// print the command line parameters for invalid command line arguments
static inline void printusage(char *prog) {
	fprintf(stderr,"Fast Ethernet CSP Process\n");
	fprintf(stderr,"Usage: %s -p [port] -out=[filename]\n",prog);
	fprintf(stderr,"If outfile is not specified, output is to screen\n");
	fprintf(stderr,"This performs one simulation with a group of SP processes\n");
}

// the simulation driver
int main(int argc, char** argv) {
	// first set the couple possible parameters
	int port = -1;
	char *outfilename = NULL;
	for (int i=1;i<argc;++i) {
		if (argv[i][0]=='-') {
			char *nextch = strchr(argv[i],'=');
			if (nextch) {
				if (argv[i][1]=='p') port = atoi(nextch+1);
				else outfilename=nextch+1;
			}
			else if (strcmp(argv[i],"-p")==0) {
				if (++i==argc) break;
				port = atoi(argv[i]);
			}
			else if (strcmp(argv[i],"-out")==0) {
				if (++i==argc) break;
				outfilename = argv[i];
			}
		}
	}
	if (port<0) {
		printusage(argv[0]);
		return 0;
	}
	// set the output file to either a log file or stdout
	FILE *outfile=NULL;
	if (outfilename) outfile = fopen(outfilename,"w");
	if (!outfile) outfile=stdout;

	// get our socket
	int fd = getlisteningsocket((unsigned short)port);
	if (fd<0) {
		fclose(outfile);
		// errors were printed in getlisteningsocket function
		return 0;
	}

	// explicitly accept the first connection
	// the first connection will tell us how big the group is
	// this is used for allocation and loop preparation
	int connfd=-1;
	while (connfd<0) {
		connfd = accept(fd,NULL,NULL);
		if (connfd<0) {
			const int errnumber = errno;
			if (connfd && (errnumber == EWOULDBLOCK || errnumber == EAGAIN)) {
				sleep(1);
				continue;
			}
			fprintf(stderr,"Error accepting first connection\n");
			fclose(outfile);
			close(fd);
			return 0;
		}
	}

	// this is the CSP input buffer
	unsigned char cspbuffer[MAXFRAMESIZE];

	// receive the first communication from the first SP
	if (!rcvbuffer(connfd,(void*)cspbuffer,INITFRAMESIZE)) {
		fprintf(stderr,"Error receiving first frame\n");
		fclose(outfile);
		close(fd);
		return 0;
	}

	// extract the values, do what checking we can
	int src_sp_id = intfrombuffer(cspbuffer);
	int dst_sp_id = intfrombuffer(cspbuffer+4);
	if (src_sp_id!=dst_sp_id) {
		fprintf(stderr,"Expected %d and %d to match in initial communication\n",src_sp_id,dst_sp_id);
		fclose(outfile);
		close(connfd);
		close(fd);
		return 0;
	}
	int numSPprocesses = intfrombuffer(cspbuffer+12);
	if (src_sp_id < 0 || src_sp_id >= numSPprocesses || numSPprocesses < 1) {
		fprintf(stderr,"Initial communication is faulty: SP ID (%d) numSPprocesses (%d)\n",src_sp_id,numSPprocesses);
		fclose(outfile);
		close(connfd);
		close(fd);
		return 0;
	}

	// setup the queue structures
	requestqueuenode *requestqueue = (requestqueuenode*)malloc(sizeof(requestqueuenode)*REQUESTQUEUESIZE);
	dataqueuenode *dataqueue = (dataqueuenode*)malloc(sizeof(dataqueuenode)*DATAQUEUESIZE);

	// -1 src_sp_id is used as the empty flag for these
	for (int i=0;i<DATAQUEUESIZE || i<REQUESTQUEUESIZE;++i) {
		if (i<DATAQUEUESIZE) dataqueue[i].src_sp_id=-1;
		if (i<REQUESTQUEUESIZE) requestqueue[i].src_sp_id=-1;
	}

	// create the sp fd array, set the fd for the connection we had and the others to negative 1
	// we hold the connected file descriptors here
	int *sp = (int*)malloc(sizeof(int)*numSPprocesses);


	// create the waiting array, SP processes will notify if they are waiting on data
	// we check the count of these to try to avoid deadlocks when other SPs notify that they are done
	int *waitsp = (int*)malloc(sizeof(int)*numSPprocesses);
	for (int i=0;i<numSPprocesses;++i) {
		if (i==src_sp_id) sp[i]=connfd; // this was our first connection
		else sp[i]=-1; // these aren't connected yet
		waitsp[i]=0; // no one is waiting for packets
	}

	// loop control vars
	// done counter, count number of SP processes that have notified 'ready for quit'
	int doneSP = 0;
	// connections needed, remaining number of connections we are expecting
	int connectionsneeded = numSPprocesses-1;
	// a round-robin style iterator
	// this iterator is used to find the next fd from the fd set after select
	// increments between 0 and numSPprocesses independent of each single loop iteration
	int roundrobin=0;

	// all data structures are ready for work, let's get to it
	while (1) { // we will break after a final unsuccessful select after everyone has said they are done
		// initialize the descriptor list for select
		fd_set fdlist;
		FD_ZERO(&fdlist);
		// if we need connections add fd to the list
		if (connectionsneeded) {
			connfd=fd; // connfd tracks the largest descriptor value for now
			FD_SET(fd,&fdlist);
		}
		// let's add any connected SP sockets to the fd set
		for (int i=0;i<numSPprocesses;++i) {
			if (sp[i]<0) continue; // nope, not that one
			if (sp[i]>connfd) connfd=sp[i]; // this one is larger
			FD_SET(sp[i],&fdlist); // add the descriptor
		}
		// wait up to 2 seconds and select one of these descriptors
		struct timeval tv = { .tv_sec=2, .tv_usec=0 };
		connfd = select(connfd+1,&fdlist,NULL,NULL,&tv);
		// zero descriptors ready or an error
		if (connfd<1) {
			// they all said they were done already, let's quit
			if (doneSP==numSPprocesses) break;
			// make sure at least one of the SPs is not waiting
			int waitingcount=0;
			for (int i=0;i<numSPprocesses;++i) {
				if (waitsp[i]) ++waitingcount;
			}
			// all of the SP processes are done or waiting, this won't work
			if (waitingcount+doneSP==numSPprocesses) {
				// set the final frame section to non-zero, (zero is for quit, non-zero is for wake up)
				ullinbuffer(cspbuffer+8,(unsigned long long)1);
				// notify every waiting SP process to stop waiting
				for (int i=0;i<numSPprocesses;++i) {
					if (!waitsp[i]) continue;
					// they almost missed the bus
					waitsp[i]=0;
					intinbuffer(cspbuffer,i);
					intinbuffer(cspbuffer+4,i);
					if (!sendbuffer(sp[i],(void*)cspbuffer,sizeof(unsigned char)*INITFRAMESIZE))
						fprintf(outfile,"CSP: Error sending SP %d notification to stop waiting\n",i);
					else
						fprintf(outfile,"CSP: Notified SP %d to stop waiting\n",i);
				}
			} // else { someone else should either wait, send data, or quit. }
			// try again
			continue;
		}
		// we need connections still, see if we have a new connection ready
		if (connectionsneeded) {
			// the listening socket has something
			if (FD_ISSET(fd,&fdlist)) {
				connfd = accept(fd,NULL,NULL);
				if (connfd>=0) {
					// initialize this connection, get their data
					if (!rcvbuffer(connfd,(void*)cspbuffer,INITFRAMESIZE)) {
						fprintf(stderr,"Error in CSP init connections, receive an initial packet\n");
						for (int x=0;x<numSPprocesses;++x) { if (sp[x]>=0) close(sp[x]); }
						fclose(outfile);
						close(fd);
						return 0;
					}
					src_sp_id=intfrombuffer(cspbuffer);
					dst_sp_id=intfrombuffer(cspbuffer+4);
					int checkgroup=intfrombuffer(cspbuffer+12);
					// validity check
					if (src_sp_id!=dst_sp_id || src_sp_id<0 || src_sp_id>=numSPprocesses || checkgroup!=numSPprocesses) {
						fprintf(stderr,"Initial communication for connection is faulty, SP %d(=%d?), numSPprocesses %d(=%d?)\n",
										src_sp_id,dst_sp_id,numSPprocesses,checkgroup);
						for (int x=0;x<numSPprocesses;++x) { if (sp[x]>=0) close(sp[x]); }
						fclose(outfile);
						close(fd);
						return 0;
					}
					// set their sp[] element and decrement the connectionsneeded counter
					sp[src_sp_id]=connfd;
					--connectionsneeded;
				}
				// go back to select another socket
				continue;
			}
		}
		// flag for if we processed a data request. If we forward data we'll re-start the loop.
		unsigned char haddata=0;
		// see if we are expecting data from this SP
		// set the connfd here to the first matching SP ID, in case we don't get data
		connfd=-1;
		for (int i=0;i<numSPprocesses;++i) {
			// round robin iterator for selecting descriptors, avoid continuing to serve one SP
			const int SP_ID=roundrobin++;
			if (roundrobin==numSPprocesses) roundrobin=0;
			// see if it is ready
			if (sp[SP_ID]<0) continue;
			if (!FD_ISSET(sp[SP_ID],&fdlist)) continue;
			// save this SP_ID, use this for the incoming request section below if no one has data
			if (connfd<0) connfd=SP_ID;
			// this SP is not waiting anymore
			if (waitsp[SP_ID]) waitsp[SP_ID]=0; // clear the wait flag
			// see if it is in the data queue
			for (int x=0;x<DATAQUEUESIZE;++x) {
				if (dataqueue[x].src_sp_id==SP_ID) {
					// this one is waiting for data and it is ready
					fprintf(outfile,"CSP: Receiving data frame from SP %d\n",SP_ID);
					// the size remaining includes the necessary header bytes
					const int thistransfer = (dataqueue[x].bytesremaining>MAXFRAMESIZE)?MAXFRAMESIZE:dataqueue[x].bytesremaining;
					// receive their data
					if (!rcvbuffer(sp[SP_ID],(void*)dataqueue[x].buffer,sizeof(unsigned char)*thistransfer))
						fprintf(stderr,"Error in CSP receive data to forward from SP %d\n",SP_ID);
					// send their data
					else if (!sendbuffer(sp[dataqueue[x].dst_sp_id],(void*)dataqueue[x].buffer,sizeof(unsigned char)*thistransfer))
							fprintf(stderr,"Error in CSP forwarding data from SP %d to SP %d\n",SP_ID,dataqueue[x].dst_sp_id);
					else
						fprintf(outfile,"CSP: Forwarded data frame (from SP %d) to SP %d\n",SP_ID,dataqueue[x].dst_sp_id);
					// decrement the amount of data we are expecting
					dataqueue[x].bytesremaining-=thistransfer;
					// not expecting any more, remove this SP from the data queue
					if (!dataqueue[x].bytesremaining) {
						dataqueue[x].src_sp_id=-1;
						// try to move something from the request queue to the data queue
						int dataqindex = shiftqueues(requestqueue,dataqueue,sp);
						if (dataqindex>=0) {
							// notify the SP that they can send this data
							intinbuffer(cspbuffer,dataqueue[dataqindex].src_sp_id);
							intinbuffer(cspbuffer+4,dataqueue[dataqindex].dst_sp_id);
							intinbuffer(cspbuffer+8,0);
							intinbuffer(cspbuffer+12,1);
							fprintf(outfile,"CSP: Moved SP %d request from request queue to data queue",dataqueue[dataqindex].src_sp_id);
							if (sendbuffer(sp[dataqueue[dataqindex].src_sp_id],(void*)cspbuffer,sizeof(unsigned char)*INITFRAMESIZE))
								fprintf(outfile,", sent acknowledgement\n");
							else {
								fprintf(outfile,", failed to send acknowledgement\n");
								dataqueue[dataqindex].src_sp_id=-1;
							}
							dataqindex = shiftqueues(requestqueue,dataqueue,sp);
						}
					}
					haddata=1;
					break;
				}
			}
			if (haddata) break;
		}
		if (haddata) continue;
		if (connfd>=0) {
			// set in the check for incoming data, this is the first file descriptor from the select
			const int SP_ID=connfd;
			// flush the log file
			fflush(outfile);
			// Read their initframe, this is some other incoming request
			if (semiblockrcv(sp[SP_ID],(void*)cspbuffer,sizeof(unsigned char)*INITFRAMESIZE)) {
				// set vals
				src_sp_id = intfrombuffer(cspbuffer);
				dst_sp_id = intfrombuffer(cspbuffer+4);
				unsigned long long datalen = ullfrombuffer(cspbuffer+8);
				// this is a signal packet from the SP for the CSP
				if (src_sp_id==dst_sp_id) {
					// this is the quit notification
					if (!datalen) {
						fprintf(outfile,"CSP: Received a ready to quit notification from SP %d",src_sp_id);
						if (datalen) fprintf(outfile," (ull: expected 0, got %llu)",datalen);
						fprintf(outfile,"\n");
						++doneSP;
					}
					// this is a waiting notification
					else {
						fprintf(outfile,"CSP: Received a notification that SP %d will wait for %llu packets\n",src_sp_id,datalen);
						// not actually counting packets
						// we turn the flag off when the SP sends something back to us
						waitsp[src_sp_id]=1;
					}
				}
				// this is a data transfer request
				// sanity check, no need to check buffers if it is a bad request
				else if (dst_sp_id<0 || dst_sp_id>=numSPprocesses) {
					fprintf(outfile,"CSP: Received request from SP %d with target SP %d\n",src_sp_id,dst_sp_id);
					fprintf(outfile,"CSP: This is a bad transmission, replying with rejection to SP %d\n",SP_ID);
					intinbuffer(cspbuffer,SP_ID);
					intinbuffer(cspbuffer+4,SP_ID+1); // just a different number than the first field
					ullinbuffer(cspbuffer+8,(unsigned long long)0);
					if (!sendbuffer(sp[SP_ID],(void*)cspbuffer,sizeof(unsigned char)*INITFRAMESIZE))
							fprintf(stderr,"CSP: Error sending rejection of invalid init packet to SP ID %d\n",SP_ID);
				}
				// the initial data request has the total data size. we set the total size here.
				// if the transfer spans multiple data frames, the SP will still hold their data queue spot
				// at least some of the math requires casting, casting all of this
				// datalen += INITFRAMESIZE * ((datalen+MAXDATASIZE-1)/MAXDATASIZE)
				else {
					datalen += (unsigned long long)
							(((unsigned long long)INITFRAMESIZE)*
							((datalen+((unsigned long long)(MAXDATASIZE-1)))
							/((unsigned long long)MAXDATASIZE))); // an init data frame per each MAXDATASIZE
					// handle the request, sendreject base val = 2
					unsigned char sendreject=2;
					// get an index if the data queue has room
					const int dataqindex = getnextdataqindex(dataqueue);
					// no room in the data queue or we are still waiting on the destination to connect
					if (dataqindex<0 || sp[dst_sp_id]<0) {
						// no room in the request queue either
						if (!queuerequest(requestqueue,src_sp_id,dst_sp_id,datalen))
							sendreject=1; // reject message
						//it was added to the request queue, don't send any response
						else sendreject=0;
					}
					else {
						// set the dataqueue vals at the index
						dataqueue[dataqindex].src_sp_id=SP_ID;
						dataqueue[dataqindex].dst_sp_id=dst_sp_id;
						dataqueue[dataqindex].bytesremaining=datalen;
					}
					// log details of the request
					fprintf(outfile,"CSP: Receive request from SP %d (%llu bytes to SP %d)\n",SP_ID,datalen,dst_sp_id);
					// we have a 1 if we send a rejection, 2 for an acceptance  ... (0 is no response)
					fprintf(outfile,"CSP: Request from SP %d is ",SP_ID);
					if (sendreject) {
						intinbuffer(cspbuffer,src_sp_id);
						intinbuffer(cspbuffer+4,dst_sp_id);
						intinbuffer(cspbuffer+8,0);
						// set a 1 in the final field for accept
						if (sendreject==2) {
							fprintf(outfile,"accepted\n");
							intinbuffer(cspbuffer+12,1);
						}
						// 0 for reject
						else {
							fprintf(outfile,"rejected\n");
							intinbuffer(cspbuffer+12,0);
						}
						// send response
						if (!sendbuffer(sp[SP_ID],(void*)cspbuffer,sizeof(unsigned char)*INITFRAMESIZE))
								fprintf(stderr,"CSP: Error sending response to SP ID %d\n",src_sp_id);
					}
					// don't send a response
					else fprintf(outfile,"queued in the request queue\n");
				}
			}
		}
		// try to move something from the request queue to the data queue
		int dataqindex = shiftqueues(requestqueue,dataqueue,sp);
		while (dataqindex>=0) {
			// notify the SP that they can send this data
			intinbuffer(cspbuffer,dataqueue[dataqindex].src_sp_id);
			intinbuffer(cspbuffer+4,dataqueue[dataqindex].dst_sp_id);
			intinbuffer(cspbuffer+8,0);
			intinbuffer(cspbuffer+12,1);
			fprintf(outfile,"CSP: Moved SP %d request from request queue to data queue",dataqueue[dataqindex].src_sp_id);
			if (sendbuffer(sp[dataqueue[dataqindex].src_sp_id],(void*)cspbuffer,sizeof(unsigned char)*INITFRAMESIZE))
				fprintf(outfile,", sent acknowledgement\n");
			else {
				fprintf(outfile,", failed to send acknowledgement\n");
				dataqueue[dataqindex].src_sp_id=-1;
			}
			dataqindex = shiftqueues(requestqueue,dataqueue,sp);
		}
	}
	// simulation is officially over.
	// for each socket send them a quit message and close the socket
	ullinbuffer(cspbuffer+8,(unsigned long long)0);
	for (int i=0;i<numSPprocesses;++i) {
		intinbuffer(cspbuffer,i);
		intinbuffer(cspbuffer+4,i);
		if (sendbuffer(sp[i],(void*)cspbuffer,sizeof(unsigned char)*INITFRAMESIZE))
			fprintf(outfile,"CSP: Sent the quit confirm to SP %d\n",i);
		else fprintf(outfile,"CSP: Error sending quit confirm to SP %d\n",i);
		shutdown(sp[i],SHUT_RDWR);
		close(sp[i]);
	}
	// clean up the last of the mess
	fprintf(outfile,"CSP: Ending simulation\n");
	fclose(outfile);
	free(sp);
	free(waitsp);
	free(requestqueue);
	free(dataqueue);
	close(fd);
	return 0;
}

