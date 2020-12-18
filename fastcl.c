#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/tcp.h> //TCP_NODELAY
#include <unistd.h>
#include <stdint.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/types.h>
#include <time.h>
#include <errno.h>
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

// max length of a line from the input cmd file
#define MAXLINELEN 128

// max number of SP processes to fork
#define FORKPROCESSLIMIT 256

// what a SP process wants to do, if they have data to send, blocked send can be masked onto value
// SENDNONE (nothing), SENDTEXT|SENDFILE (send data), SENDBLOCKED (wait for ok), SENDFINISHED (no more cmd file)
enum spstatus { SENDNONE=0, SENDTEXT=0x1, SENDFILE=0x10, SENDBLOCKED=0x100, SENDFINISHED=0x1000 };

// the data packet struct
// this is used for outgoing communications, it is pre-packaged to await the ok from the CSP
// it has its own buffer (set ahead of time)
// the dst_sp_id, sequence number (from cmd file), and bufferlen
// the next send (of buffer) will be of size (bufferlen)
// the sizeremaining is the (file)size remaining, in case it doesn't all fit in one data frame
typedef struct datapacket {
	unsigned char buffer[MAXFRAMESIZE];
	int dst_sp_id;
	int seqnum;
	int bufferlen;
	unsigned long long sizeremaining;
}datapacket;

// print usage info, called for bad command line arguments
static inline void printusage(char *prog) {
	fprintf(stderr,"Fast Ethernet Station Process Launcher\n");
	fprintf(stderr,"Help (this information): -h\n");
	fprintf(stderr,"Interactive mode: %s -n 1 127.0.0.1:52528\n",prog);
	fprintf(stderr,"Input file usage: %s -n 5 127.0.0.1:52528 -in=commands.txt\n",prog);
	fprintf(stderr,"(Input file format specified in README)\n");
	fprintf(stderr,"-n X specifies to launch X SP processes (processes are numbered from zero)\n");
	fprintf(stderr,"Specify output location: %s -n 1 127.0.0.7:52528 -in=input -out=logprefix\n",prog);
	fprintf(stderr,"Output files then created as: logprefix0.log, logprefix1.log, ..., where the number is the SP number\n");
}

// station process (SP) driver program
// takes a number of SP processes to launch
// connects to ip:port specified in args
// receives input from file(s) or console
// the input files are given by a prefix, the SP ID is appended to the prefix for the input filename
// output is to stdout unless specified, if specified it is also by prefix, output filenames will have each SP ID appended
// each SP process is independent once forked, only communications are through the CSP
int main(int argc, char** argv) {
	// set up initial vars, parse command line args
	char *logfilename=NULL, *switch_ip=NULL, *inputfilename=NULL;
	int numprocesses=-1, port = -1;

	// I'm just using a constant value
/** Seed the random **/
	srand(1337);
/**               **/

	// parse the command line args
	for (int i=1;i<argc;++i) {
		char *chrptr = argv[i];
		if (*chrptr=='-') {
			++chrptr;
			if (*chrptr=='h') {
				printusage(argv[0]);
				return 0;
			}
			if (*chrptr=='n') {
				++chrptr;
				if (*chrptr=='\0') chrptr=argv[++i];
				else if (*chrptr==' ') ++chrptr;
				numprocesses=atoi(chrptr);
				if (numprocesses<1) numprocesses=1;
				if (numprocesses>FORKPROCESSLIMIT) numprocesses=FORKPROCESSLIMIT;
			}
			else {
				char *nextchr = strchr(chrptr,'=');
				if (nextchr) {
					*nextchr='\0';
					if (strcmp(chrptr,"in")==0)
						inputfilename=nextchr+1;
					else if (strcmp(chrptr,"out")==0)
						logfilename=nextchr+1;
					else {
						fprintf(stderr,"Error: expected one of \"-h\", \"-n 1\", \"-in=input\", \"-out=output\"\n");
						printusage(argv[0]);
						return 0;
					}
				}
			}
		}
		else { // server info is without -arg=
			switch_ip=strchr(chrptr,':');
			if (!switch_ip) {
				fprintf(stderr,"Error: expected \"ip:port\"\n");
				return 0;
			}
			*switch_ip='\0';
			port=atoi(switch_ip+1);
			switch_ip=chrptr;
		}
	}
	if (!switch_ip || port<0 || numprocesses<1) {
		fprintf(stderr,"Error: ");
		if (!switch_ip) {
			fprintf(stderr,"no valid ip");
			if (port<0 || numprocesses<1)
				fprintf(stderr,", ");
			else fprintf(stderr,"\n");
		}
		if (port<0) {
			fprintf(stderr,"no valid port");
			if (numprocesses<1) fprintf(stderr,", ");
			else fprintf(stderr,"\n");
		}
		if (numprocesses<1) fprintf(stderr,"no SP process count\n");
		return 0;
	}

	// command line arg options are set, set the CSP struct sockaddr_in before we fork
	struct sockaddr_in addr;
	memset((void*)&addr,0,sizeof(struct sockaddr_in));
	addr.sin_family = AF_INET;
	if (inet_pton(AF_INET,switch_ip,&addr.sin_addr)<=0) {
		fprintf(stderr,"Error: unable to convert ip \"%s\"\n",switch_ip);
		return 0;
	}
	addr.sin_port=htons((unsigned short)port);

	// fork each child SP process
	// everyone knows their SP number
	int SP_ID=-1;
	// the main process saves the child PIDs
	pid_t *SPs = (pid_t*)malloc(sizeof(pid_t)*numprocesses);
	for (int i=0;i<numprocesses;++i) {
		pid_t pid = fork();
		if (pid<0) {
			fprintf(stderr,"Error: unable to fork SP %d of %d (id %d)\n",i+1,numprocesses,i);
			return 0;
		}
		else if (!pid) { // child process
			SP_ID=i;
			break;
		}
		// parent
		SPs[i]=pid;
	}

	// parent process, wait for all SP processes before exiting
	if (SP_ID<0) {
		siginfo_t info; // ignoring this and any other response
		for (int i=0;i<numprocesses;++i) {
			waitid(P_PID,(id_t)SPs[i],&info,WEXITED); // blocking until exit
		}
		fprintf(stdout,"Parent process normal exit\n");
		// simulation is over already !
		free(SPs);
		return 0;
	}

	// forked processes, the SP processes
	free(SPs); // (they don't need an incomplete PID list)

	// set our input file
	FILE *cmdfile;
	if (inputfilename) {
		// if an input file prefix is specified it must open successfully
		char *myinputfilename = (char*)malloc(sizeof(char)*(strlen(inputfilename)+10)); //allows 9 chars for SP ID
		sprintf(myinputfilename,"%s%d",inputfilename,SP_ID);
		if (!(cmdfile=fopen(myinputfilename,"r"))) {
			fprintf(stderr,"Error: SP ID %d unable to open input file %s !\n",SP_ID,myinputfilename);
			free(myinputfilename);
			return 0;
		}
		free(myinputfilename);
	}
	else {
		// no input file, if the SP_ID is not zero then quit
		if (SP_ID) {
			fprintf(stderr,"Error: SP ID %d refusing to make more than 1 interactive SP process\n",SP_ID);
			return 0;
		}
		numprocesses=1;
		// input from stdin
		cmdfile = stdin;
		fprintf(stdout,"SP ID 0 (interactive mode)\t(type \'help\' for a list of commands)\n");
	}

	// set output file
	FILE *logfile;
	if (logfilename && cmdfile!=stdin) {
		// if an output file prefix is specified it must open successfully
		char *mylogfilename = (char*)malloc(sizeof(char)*(strlen(logfilename)+14)); // allows 9 chars for SP ID
		sprintf(mylogfilename,"%s%d.log",logfilename,SP_ID);
		if (!(logfile=fopen(mylogfilename,"w"))) {
			fprintf(stderr,"Error: SP ID %d unable to open log file %s\n",SP_ID,mylogfilename);
			free(mylogfilename);
			return 0;
		}
		free(mylogfilename);
	}
	// if no output prefix is specified we use stdout, (allowing multiple SP IDs to print to stdout)
	else logfile = stdout;

	// connect to the CSP, the Communication Switch Process
	// first, get a socket
	int fd = socket(AF_INET,SOCK_STREAM | SOCK_NONBLOCK,IPPROTO_TCP);
	if (fd<0) {
		fprintf(stderr,"Error: SP ID %d unable to get a socket\n",SP_ID);
		fclose(cmdfile);
		return 0;
	}
	// then, connect
	unsigned char failcount=0; // also using this var later for the CSP rejection counter
	while ((connect(fd,(const struct sockaddr*)&addr,sizeof(struct sockaddr)))<0) {
		sleep(1);
	}

	// I had issues (with many processes fighting for attention) of "Connection reset by peer"
	int optval=1;
	// enable the KEEPALIVE flag at the socket level
	setsockopt(fd,SOL_SOCKET,SO_KEEPALIVE,(const void*)&optval,sizeof(int));
// (the below TCP options are labelled in the docs as not for portable code)
// I needed to use these to handle 10 processes and 1 of each CSP queue type . . .
// These options stopped the mid-simulation connection failures (connection reset by peer)
// I suppose a reconnect routine could probably fix this without using these options . . .
	// set the delay for the first KEEPALIVE to 1 second
	int firstkeepalivedelay=1;
	setsockopt(fd,IPPROTO_TCP,TCP_KEEPIDLE,&firstkeepalivedelay,sizeof(int));
	// set the interval between KEEPALIVE messages to 3 seconds
	int keepaliveinterval=3;
	setsockopt(fd,IPPROTO_TCP,TCP_KEEPINTVL,&keepaliveinterval,sizeof(int));
	// set the max number of KEEPALIVE messages to 240
	int maxkeepalives=240;
	setsockopt(fd,IPPROTO_TCP,TCP_KEEPCNT,&maxkeepalives,sizeof(int));

	// our tcp input buffer, this is where TCP input goes
	unsigned char tcpinbuffer[MAXFRAMESIZE];
	// the outbound data packet (it also has "unsigned char .buffer[MAXFRAMESIZE]")
	datapacket outpacket = { .dst_sp_id=-1, .seqnum=1, .bufferlen=0, .sizeremaining=(unsigned long long)0 };

	// send the CSP our SP ID and the number of SP processes it should expect
	intinbuffer(tcpinbuffer,SP_ID);
	intinbuffer(tcpinbuffer+4,SP_ID);
	ullinbuffer(tcpinbuffer+8,(unsigned long long)numprocesses);
	if (!sendbuffer(fd,(void*)tcpinbuffer,sizeof(unsigned char)*INITFRAMESIZE)) {
		fprintf(stderr,"SP %d: CSP connection was closed before first communication\n",SP_ID);
		fclose(cmdfile);
		fclose(logfile);
		return 0;
	}

	// I have found it is helpful (with my single-machine testing)
	// to sleep here for a second or two
	sleep(1);

	// counter for number rejections, each retransmission attempt may wait longer than the previous
	failcount=0;
	// flag for whether to resend request
	unsigned char resendrequest=0;
	// counter for number of packets waiting to be received
	int waitpackets=0;
	int sendtype=SENDNONE; //SENDNONE=0, SENDTEXT=0x1, SENDFILE=0x10, SENDBLOCKED=0x100
	// a file to be sent, set through input commands
	FILE *sendfile=NULL;
	while (1) {
		// attempt to read from the server, if there is nothing we'll skip this
		// if we are waiting on packets or don't have any input use the sleep/blocking rcvbuffer
		if (((sendtype==SENDFINISHED || waitpackets) && rcvbuffer(fd,(void*)tcpinbuffer,sizeof(unsigned char)*INITFRAMESIZE))
			|| (sendtype!=SENDFINISHED && !waitpackets && (semiblockrcv(fd,(void*)tcpinbuffer,sizeof(unsigned char)*INITFRAMESIZE)))) {
			// see what the packet says
			int srcaddr = intfrombuffer(tcpinbuffer);
			int dstaddr = intfrombuffer(tcpinbuffer+4);
			// the SP processes more often deal with the last 8 bytes as 2 integers
			const int packetnum = intfrombuffer(tcpinbuffer+8);
			const int lastfield = intfrombuffer(tcpinbuffer+12);
			// server simulation response
			if (srcaddr==dstaddr) {
				// quit simulation
				if (!lastfield) {
					fprintf(logfile,"SP %d: Received ",SP_ID);
					if (srcaddr!=SP_ID || packetnum || lastfield) fprintf(logfile,"in");
					fprintf(logfile,"valid quit response from CSP\n");
					break;
				}
				// stop waiting for packets
				fprintf(logfile,"SP %d: Received notification from CSP to stop waiting for packets\n",SP_ID);
				waitpackets=0;
				continue;
			}
			// it is a response to a request
			if (srcaddr==SP_ID) {
				fprintf(logfile,"SP %d: Received ",SP_ID);
				if (lastfield==0) {
					fprintf(logfile,"reject");
					// we've had 3 retries, drop this request.
					if (failcount++ == 3) {
						// clear counter/state vars
						failcount=0;
						sendtype=SENDNONE;
						outpacket.bufferlen=0;
						outpacket.sizeremaining=0;
					}
					// send another request next time
					else resendrequest=1;
				}
				else { // if lastfield>0
					// send the data packet
					fprintf(logfile,"ok");
					// remove SENDBLOCKED so we can send
					if (sendtype&SENDBLOCKED) sendtype-=SENDBLOCKED;
					failcount=0;
				}
				fprintf(logfile," reply from CSP to send data frame %d to SP %d\n",outpacket.seqnum,dstaddr);
				continue;
			}
			// it is incoming data, get the data
			fprintf(logfile,"SP %d: ",SP_ID);
			if (!rcvbuffer(fd,(void*)tcpinbuffer,sizeof(unsigned char)*lastfield))
				fprintf(logfile,"Failed to receive");
			else
				fprintf(logfile,"Received");
			fprintf(logfile," packet %d (%d bytes) from SP %d\n",packetnum,lastfield,srcaddr);
			// we are waiting to receive packets, decrement that counter
			if (waitpackets) {
				if (--waitpackets==0) fprintf(logfile,"SP %d: Finished waiting for data frames\n",SP_ID);
				continue;
			}
		}
		// we are finished with the cmd input or are waiting to receive packets
		if (sendtype==SENDFINISHED || waitpackets) {
			if (rand()%2) sleep(1); // sleep for 1 second half the time
			continue;
		}
		// we are going to resend the data transmission request
		if (resendrequest) {
			// a simple BEBO backoff. time slots are each one 1 second
			// after the nth failure (failcount), sleep for between (0) and ((2^n)-1) time slots
			// 1st -> 0-1, 2nd-> 0-3, 3rd-> 0-7
			int sleepval = rand()%(2<<(failcount-1));
			if (sleepval) sleep(sleepval);
			// if sendtype is SENDFILE these fields may not be correct for the initial request
			const unsigned long long lastfield = ullfrombuffer(outpacket.buffer+8);
			// the CSP wants the total data size (excluding frame headers)
			if (sendtype==SENDFILE)
				ullinbuffer(outpacket.buffer+8,outpacket.sizeremaining);
			if (!sendbuffer(fd,(void*)outpacket.buffer,sizeof(unsigned char)*INITFRAMESIZE)) {
				fprintf(logfile,"SP %d: Resend attempt %u, failed to resend request frame to CSP\n",SP_ID,failcount);
				// shouldn't get an error, just cancel this request
				outpacket.sizeremaining=0;
				outpacket.bufferlen=0;
				sendtype=SENDNONE;
				failcount=0;
			}
			else {
				fprintf(logfile,"SP %d: Resent request to send frame %d, (%llu bytes) to SP %d\n",
					SP_ID,outpacket.seqnum,outpacket.sizeremaining,outpacket.dst_sp_id);
				// restore whatever was in there if we overwrote it
				if (sendtype==SENDFILE)
					ullinbuffer(outpacket.buffer+8,lastfield);
			}
			// wait for a response, do not resend again
			resendrequest=0;
			continue;
		}
		// have a formed outgoing packet ready to go
		if (outpacket.bufferlen) {
			// we are blocked from sending, restart the loop
			if (sendtype&SENDBLOCKED) {
				if (rand()%2) sleep(1); // sleep for 1 second half the time
				continue;
			}
			// we are going to send the outgoing data
			fprintf(logfile,"SP %d: ",SP_ID);
			if (!sendbuffer(fd,(void*)outpacket.buffer,sizeof(unsigned char)*outpacket.bufferlen))
				fprintf(logfile,"Error sending");
			else fprintf(logfile,"Sent");
			fprintf(logfile," data packet (%d bytes) to SP %d\n",outpacket.bufferlen,outpacket.dst_sp_id);
			// we sent ((bufferlen)-(headersize)) bytes of the data remaining
			outpacket.sizeremaining-=(unsigned long long)(outpacket.bufferlen-INITFRAMESIZE);
			// there is no more length in the buffer
			outpacket.bufferlen=0;
			// there is also no more remaining data to send
			if (!outpacket.sizeremaining)
				sendtype=SENDNONE;
			// restart the loop
			continue;
		}
		// there was no output packet, nothing is blocking, try to read the cmd file
		// we can get a "send" or "wait" command from the cmd file, this is handled here
		// requests are sent when/if we create an outpacket (also set the SENDBLOCKED flag)
		// wait packet counters are set immediately
		if (cmdfile) {
			// still have file remaining (sizeremaining only happens with SENDFILE)
			if (outpacket.sizeremaining) {
				// read more input file into buffer
				// first two fields of output buffer (src and dst) remain the same (as they were already)
				// update the packet counter and the size of the next frame
				const int packetcounter = intfrombuffer(outpacket.buffer+8)+1;
				intinbuffer(outpacket.buffer+8,packetcounter);
				// minimum size of a transmission with no data
				outpacket.bufferlen=INITFRAMESIZE;
				// read until we get to EOF
				while ((outpacket.buffer[outpacket.bufferlen]=fgetc(sendfile))!=EOF) {
					// break if we fill the buffer
					if (++outpacket.bufferlen == MAXFRAMESIZE) break;
					// break if we read the final byte
					if (outpacket.bufferlen-INITFRAMESIZE == outpacket.sizeremaining) break;
				}
				// if we are at EOF close the file and set the pointer to NULL
				if (feof(sendfile)) {
					fclose(sendfile);
					sendfile=NULL;
				}
				const int thistransfersize = outpacket.bufferlen-INITFRAMESIZE;
				// we collected bytes to send
				if (thistransfersize)
					intinbuffer(outpacket.buffer+12,thistransfersize);
				// there was nothing left in the file to send
				else {
					outpacket.sizeremaining=0;
					outpacket.bufferlen=0;
				}
				// restart loop, the next packet is ready
				continue;
			}
			// no pending outpacket, no pending sendfile
			// still may have some cmd file, try to read a command
			// line buffer for text input of cmd file
			char linebuffer[MAXLINELEN];
			memset((void*)linebuffer,0,sizeof(char)*MAXLINELEN);
			while (!feof(cmdfile)) {
				// read a line of file
				char *cmdline = fgets(linebuffer,MAXLINELEN,cmdfile);
				// cmdline is NULL, must have hit EOF
				if (!cmdline) break;
				// skip lines that begin with these characters
				if (linebuffer[0]=='\0'||linebuffer[0]=='\n'||linebuffer[0]=='#')
					continue;
				// we have a string length, lets use this line
				if (strlen(linebuffer)) break;
			}
			// if the file is EOF close the file and set to NULL to skip this loop next time
			if (feof(cmdfile)) {
				fclose(cmdfile);
				cmdfile=NULL;
			}
			// we have a line to parse
			if (linebuffer[0] && strlen(linebuffer)>1) { // minimum cutoff for valid lines*
				// walking two pointers up with strchr
				// input format is VERY strict (for placement of a few magic characters)
				// if parsing doesn't find a match the line will have no effect
				char *sendchar = NULL;
				char *nextch = strchr(linebuffer,' ');
				if (nextch) {
					*nextch++='\0';
//Wait for receiving 1 frame
//Wait for receiving 2 frames # (plural 's' / etc doesn't matter with how this is parsed)
					// need to set the counter to wait for data frames
					if (strcmp(linebuffer,"Wait")==0) {
						char *endch = strchr(nextch,'g');
						if (endch) {
							endch+=2;
							nextch=strchr(endch,' ');
							if (nextch) {
								*nextch='\0';
								waitpackets+=atoi(endch);
								// we can already do zero
								if (!waitpackets) continue;
								fprintf(logfile,"SP %d: Entering wait to receive %d data frames\n",SP_ID,waitpackets);
								// notify the CSP that we will be waiting
								intinbuffer(outpacket.buffer,SP_ID);
								intinbuffer(outpacket.buffer+4,SP_ID);
								ullinbuffer(outpacket.buffer+8,(unsigned long long)waitpackets);
								// the CSP will wake us up if every other SP is ready to quit (no one else is expected to send data)
								if (!sendbuffer(fd,outpacket.buffer,sizeof(unsigned char)*INITFRAMESIZE))
									fprintf(logfile,"SP %d: Error notifying CSP of wait for %d packets\n",SP_ID,waitpackets);
							}
						}
					}
// # send frame number to sp 2
// Frame 1, To SP 2
// # send text to sp 2 (it just reads the rest of the line up to maxdatasize)
// Frame 1, To SP 2 text to send
// # send file to sp 2 ( not yet implemented yet )
// Frame 1, To SP 2 $sendfile.txt
					// send data frame
					else if (strcmp(linebuffer,"Frame")==0) {
						char *endch = strchr(nextch,',');
						if (endch) {
							*endch='\0';
							// nextch is the first char after the first space
							// packet number from input
							outpacket.seqnum = atoi(nextch);
							nextch = strchr(endch+1,'P');
							if (nextch) {
								// nextch is two chars after the 'P'
								nextch+=2;
								// endch is the space after
								char *endch = strchr(nextch,' ');
								//	   endch points v
								// "Frame 1, To SP 2 xxx"
								// nextch points   ^
								if (endch) {
									*endch='\0';
									// dst sp id
									outpacket.dst_sp_id = atoi(nextch);
									sendchar=endch+1;
									// "Frame 1, To SP 2 $./inputfile.txt
									if (*sendchar=='$') {
										if (sendchar[1]=='\n' || sendchar[1]=='\0')
											sendtype=SENDTEXT;
										// send file if text exists after the '$'
										else {
											++sendchar;
											sendtype=SENDFILE;
										}
									}
									// Frame 1, To SP 2 words to send
									else sendtype=SENDTEXT;
								}
								// no trailing text after "SP 2"
								// "Frame 1, To SP 2"
								else {
									outpacket.dst_sp_id = atoi(nextch);
									sendtype=SENDTEXT;
								}
								// setup the outpacket buffer, src->dst
								intinbuffer(outpacket.buffer,SP_ID);
								intinbuffer(outpacket.buffer+4,outpacket.dst_sp_id);
								// start of the data segment, the data size indicates size of databuffer ready to send
								outpacket.bufferlen=INITFRAMESIZE;
								// sending text from input file
								if (sendtype==SENDTEXT) {
									// send the rest of the line
									if (endch) {
										while (*sendchar!='\0') {
											outpacket.buffer[outpacket.bufferlen++]=*sendchar;
											++sendchar;
										}
									}
									// no line, just send the frame number
									else {
										char seqnumberchar[16]; // large enough for any int
										sprintf(seqnumberchar,"%d",outpacket.seqnum);
										// the data size
										for (int i=0;seqnumberchar[i]!='\0';++i) {
											outpacket.buffer[outpacket.bufferlen++]=seqnumberchar[i];
										}
									}
									// the bytes after the header are all there will be
									outpacket.sizeremaining=((unsigned long long)outpacket.bufferlen)-((unsigned long long)INITFRAMESIZE);
									// size of full data (excluding headers)
									ullinbuffer(outpacket.buffer+8,outpacket.sizeremaining);
								}
								// sending bytes from a file
								else if (sendtype==SENDFILE) {
									// turn any trailing newline into a null terminator
									for (int i=0;sendchar[i]!='\0';++i) {
										if (sendchar[i]=='\n') {
											sendchar[i]='\0';
											break;
										}
									}
									// try to open the filename
									sendfile = fopen(sendchar,"rb");
									if (!sendfile) {
										// if we couldn't open the file we send a text message
										sendtype=SENDTEXT;
										// if we have a string we can send an error string
										if (strlen(sendchar)>0) {
											//Error opening: 
											char *ferrormsg = (char*)malloc(sizeof(char)*(strlen(sendchar)+16));
											sprintf(ferrormsg,"%s %s","Error opening:",sendchar);
											// the data size is the string length of this string
											outpacket.sizeremaining=(unsigned long long)strlen(ferrormsg);
											// put the string in the buffer
											for (int i=0;ferrormsg[i]!='\0';++i) {
												//bufferlen was set to INITFRAMESIZE right before this 'if'
												outpacket.buffer[outpacket.bufferlen++]=ferrormsg[i];
											}
										}
										// no strlen ?
										else {
											// let's send the '$' character
											outpacket.sizeremaining=(unsigned long long)1;
											outpacket.buffer[outpacket.bufferlen++]='$';
										}
									}
									else {
										// we could open the file, get the filesize
										fseek(sendfile,0,SEEK_END);
										outpacket.sizeremaining = ftell(sendfile);
										// it was an empty file, let's just skip this one then.
										if (!outpacket.sizeremaining) {
											fclose(sendfile);
											sendfile=NULL;
											outpacket.sizeremaining=0;
											outpacket.bufferlen=0;
											sendtype=SENDNONE;
											continue;
										}
										rewind(sendfile);
										// we have the filesize, read up to (filesize) or (MAXFRAMESIZE) bytes
										while ((outpacket.buffer[outpacket.bufferlen]=fgetc(sendfile))) {
											// frame is full
											if (++outpacket.bufferlen == MAXFRAMESIZE) break;
											// file bytes all read
											if (outpacket.bufferlen-INITFRAMESIZE==outpacket.sizeremaining) break;
										}
										// no file left, close it and set the pointer to null
										if (feof(sendfile)) {
											fclose(sendfile);
											sendfile=NULL;
										}
									}
									// all SENDFILE conditions above have set the .sizeremaining
									ullinbuffer(outpacket.buffer+8,outpacket.sizeremaining);
								}
								// send the initial request (initial packet to CSP is ready)
								if (sendtype!=SENDNONE) {
									// sanity check, this means there is no data
									if (outpacket.bufferlen==INITFRAMESIZE) {
										outpacket.bufferlen=0;
										outpacket.sizeremaining=0;
										// don't know how both of these could happen
										if (sendtype==SENDFILE && sendfile) {
											fclose(sendfile);
											sendfile=NULL;
										}
										sendtype=SENDNONE;
										// let's get out of here
										continue;
									}
									// send the request buffer to the CSP
									fprintf(logfile,"SP %d: Frame %d, request to send %llu bytes to SP %d\n",
										SP_ID,outpacket.seqnum,outpacket.sizeremaining,outpacket.dst_sp_id);
									if (!sendbuffer(fd,(void*)outpacket.buffer,sizeof(unsigned char)*INITFRAMESIZE)) {
										fprintf(logfile,"SP %d: Error sending data request frame to CSP\n",SP_ID);
										// shouldn't get an error, cancel the request
										if (sendtype==SENDFILE && sendfile) {
											fclose(sendfile);
											sendfile=NULL;
										}
										outpacket.sizeremaining=0;
										outpacket.bufferlen=0;
										sendtype=SENDNONE;
									}
									// request sent
									else {
										// put the block on
										sendtype|=SENDBLOCKED;
										// if (sendtype==SENDFILE) (the length will be greater only with files)
										// ensure correct (file size+packet #) in first frame
										// the frame to the CSP has the total size, for a file this can be multiple data frames
										// each frame received by an SP contains an int in the 4th position indicating current size
										if (outpacket.bufferlen-INITFRAMESIZE<outpacket.sizeremaining) {
											// this transmission will be broken up over multiple transfers
											fprintf(logfile,"SP %d: Will send file in chunks of %d bytes\n",SP_ID,MAXDATASIZE);
											intinbuffer(outpacket.buffer+8,0);
											intinbuffer(outpacket.buffer+12,outpacket.bufferlen-INITFRAMESIZE); // MAXDATASIZE
										}
									}
									// we have not failed so far
									failcount=0;
									resendrequest=0;
								}
							}
						}
					}
				}
			}
			// we read a line from the cmd file, and may have done something
			continue;
		}
		// to reach this point there is nothing left to do except receive data
		// notify the CSP we are done with the input file and will not be sending anything else
		if (!sendtype) {
			sendtype=SENDFINISHED;
			intinbuffer(outpacket.buffer,SP_ID);
			intinbuffer(outpacket.buffer+4,SP_ID);
			ullinbuffer(outpacket.buffer+8,0);
			fprintf(logfile,"SP %d: Notifying CSP ready to quit\n",SP_ID);
			if (!sendbuffer(fd,(void*)outpacket.buffer,sizeof(unsigned char)*INITFRAMESIZE)) {
					fprintf(logfile,"SP %d: Error sending quit packet to CSP\n",SP_ID);
					fclose(cmdfile);
					fclose(logfile);
					return 0;
			}
		}
	}
	// simulation is officially over.
	fprintf(logfile,"SP %d: Ending simulation\n",SP_ID);
	// close up shop
	fclose(logfile);
	// these cases shouldn't happen
	if (cmdfile) fclose(cmdfile);
	if (sendfile) fclose(sendfile);
	// shut it down
	shutdown(fd,SHUT_RD);
	close(fd);
	return 0;
}
