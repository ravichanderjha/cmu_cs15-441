/******************************************************************************
* echo_server.c                                                               *
*                                                                             *
* Description: This file contains the C source code for an echo server.  The  *
*              server runs on a hard-coded port and simply write back anything*
*              sent to it by connected clients.  It does not support          *
*              concurrent clients.                                            *
*                                                                             *
* Authors: Athula Balachandran <abalacha@cs.cmu.edu>,                         *
*          Wolf Richter <wolf@cs.cmu.edu>                                     *
*                                                                             *
*******************************************************************************/

#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netdb.h>

#define PORT "9999"
#define BUF_SIZE 4096
#define MAX_CONN 10

int close_socket(int sock)
{
    if (close(sock))
    {
        fprintf(stderr, "Failed closing socket.\n");
        return 1;
    }
    return 0;
}

// get sockaddr, IPv4 or IPv6:
void *get_in_addr(struct sockaddr *sa)
{
	if (sa->sa_family == AF_INET) {
		return &(((struct sockaddr_in*)sa)->sin_addr);
	}

	return &(((struct sockaddr_in6*)sa)->sin6_addr);
}


int main(int argc, char* argv[])
{
	char greet[] = "Hello Sir! How are you?";
	int greet_size = sizeof(greet);
	fd_set master;
	fd_set read_fds;
	int fdmax;
	
	int listener;
	int newfd;
	struct sockaddr_storage remoteaddr; // client address
	socklen_t addrlen;
	
	char buf[BUF_SIZE];
	int nbytes;

	char remoteIP[INET6_ADDRSTRLEN];

	int yes=1;        // for setsockopt() SO_REUSEADDR, below
	int i, rv;

	struct addrinfo hints, *ai, *p;

	FD_ZERO(&master);    // clear the master and temp sets
	FD_ZERO(&read_fds);

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;

	if ((rv = getaddrinfo(NULL, PORT, &hints, &ai)) != 0)
	{
		fprintf(stderr, "selectserver: %s\n", gai_strerror(rv));
		exit(1);
	}

	for(p = ai; p != NULL; p = p->ai_next)
	{
	    	listener = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if (listener < 0)
		{ 
			continue;
		}
		
		// lose the pesky "address already in use" error message
		setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(int));

		if (bind(listener, p->ai_addr, p->ai_addrlen) < 0) {
			close(listener);
			continue;
		}

		break;
	}

	// if we got here, it means we didn't get bound
	if (p == NULL) {
		fprintf(stderr, "selectserver: failed to bind\n");
		exit(2);
	}
	freeaddrinfo(ai); // all done with this

	// listen
	if (listen(listener, MAX_CONN) == -1)
	{
		perror("listen");
		exit(3);
	}

	// add the listener to the master set
	FD_SET(listener, &master);

	// keep track of the biggest file descriptor
	fdmax = listener; // so far, it's this one

	// main loop
	for(;;)
	{
		read_fds = master; // copy it
		if (select(fdmax+1, &read_fds, NULL, NULL, NULL) == -1) {
		    perror("select");
		    exit(4);
		}
		for(i = 0; i <= fdmax; i++)
		{
			if (FD_ISSET(i, &read_fds))
			{
				if (i == listener)
				{
					addrlen = sizeof remoteaddr;
					newfd = accept(listener,(struct sockaddr *)&remoteaddr, &addrlen);
					if (newfd == -1)
					{
					        perror("accept");
					}
					else
					{
						FD_SET(newfd, &master); // add to master set
						if (newfd > fdmax)
						{    // keep track of the max
						    fdmax = newfd;
						}
						printf("selectserver: new connection from %s on socket %d\n", inet_ntop(remoteaddr.ss_family, get_in_addr((struct sockaddr*)&remoteaddr), remoteIP, INET6_ADDRSTRLEN), newfd);
					}
				}
				else
				{
					// handle data from a client
					if ((nbytes = recv(i, buf, sizeof buf, 0)) <= 0)
					{
						if (nbytes == 0)
						{
							// connection closed
							printf("selectserver: socket %d hung up\n", i);
						}
						else
						{
							perror("recv");
						}
						close(i); // bye!
						FD_CLR(i, &master); // remove from master set
					}
					else
					{
						if (send(i, greet, greet_size, 0) == -1)
							perror("send");
					}
				}			
			}
		}
	} // END for(;;)--and you thought it would never end!
	
	return 0;
}
