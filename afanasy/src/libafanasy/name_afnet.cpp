#include "name_af.h"

#ifndef WINNT
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#define closesocket close
#endif

#include "../include/afanasy.h"

#include "address.h"
#include "environment.h"
#include "msg.h"
#include "msgstat.h"

#define AFOUTPUT
#undef AFOUTPUT
#include "../include/macrooutput.h"

af::MsgStat mgstat;

/// Read data from file descriptor. Return bytes than was written or -1 on any error and prints an error in \c stderr.
int readdata( int fd, char* data, int data_len, int buffer_maxlen)
{
	AFINFA("readdata: trying to recieve %d bytes.\n", data_len);
	int bytes = 0;
	while( bytes < data_len )
	{
#ifdef WINNT
		int r = recv( fd, data+bytes, buffer_maxlen-bytes, 0);
#else
		int r = read( fd, data+bytes, buffer_maxlen-bytes);
#endif
		if( r < 0)
		{
			AFERRPE("readdata: read");
			return -1;
		}
		AFINFA("readdata: read %d bytes.\n", r);
		if( r == 0) return bytes;
		bytes += r;
	}

	return bytes;
}

/// Write data to file descriptor. Return \c false on any arror and prints an error in \c stderr.
bool writedata( int fd, const char * data, int len)
{
	int written_bytes = 0;
	while( written_bytes < len)
	{
#ifdef WINNT
		int w = send( fd, data+written_bytes, len, 0);
#else
		int w = write( fd, data+written_bytes, len);
#endif
		if( w < 0)
		{
			AFERRPE("name_afnet.cpp writedata:");
			return false;
		}
		written_bytes += w;
	}
	return true;
}

// Return header offset or -1, if it was not recognized. GET request processed here.
int processHeader( af::Msg * io_msg, int i_bytes, int i_desc)
{
//printf("\nReceived %d bytes:\n", i_bytes);
	char * buffer = io_msg->buffer();
	int offset = 0;

	// Process HTTP header:
	if( strncmp( buffer, "POST", 4) == 0 )
	{
		//writedata( 1, buffer, i_bytes); write(1,"\n",1);
		offset = 4;
		int size;
		bool header_processed = false;
		for( offset = 4; offset < i_bytes; offset++)
		{
			// Look for line end:
			if( buffer[offset] == '\n' )
			{
				// Go to line begin:
				offset++;
				if( offset == i_bytes )
					break;

				// If header found, body can start in the same data packet:
				if( header_processed && ( buffer[offset] == '{' ))
				{
					//write(1,"\nBODY FOUND:\n", 15);
					//write(1, buffer+offset, i_bytes - offset);
					//write(1,"\n",1);
					break;
				}

				// Look for a special header:
				if( strncmp("AFANASY: ", buffer+offset, 9) == 0)
				{
					//printf("\nAFANASY FOUND:\n");
					offset += 9;
					if( 1 == sscanf( buffer + offset, "%d", &size))
					{
						//printf("\nHEADER FOUND: size=%d\n", size);
						header_processed = true;
					}
					else return -1;
				}
			}
		}

		// If header found, construct message:
		if( header_processed )
		{
			io_msg->setHeader( af::Msg::THTTP, size, offset, i_bytes);
			return offset;
		}

		// Header not recongnized:
		return -1;
	}

	// Simple header for JSON (used for example in python api and afcmd)
	if( strncmp("AFANASY", buffer, 7) == 0 )
	{
		offset += 7;
//writedata( 1, buffer+offset, i_bytes);
		int size;
		int num = sscanf( buffer + offset, "%d", &size);
//printf("\n sscanf=%d\n",num);
		if( num == 1 )
		{
			while( ++offset < i_bytes )
			{
				if( strncmp( buffer+offset, "JSON", 4) == 0)
				{
					offset += 3;
					while( ++offset < i_bytes )
					{
						if( buffer[offset] == '{' )
						{
							//printf("FOUND: size=%d Offset=%d:\n", size, offset);
							//write(1, buffer, offset);
							//write(1, buffer+offset, i_bytes - offset);
							//write(1,"\n",1);
							io_msg->setHeader( af::Msg::TJSON, size, offset, i_bytes);
							//return false;
							//io_msg->stdOutData();
							return offset;
						}
					}
				}
			}

			// Header not recongnized:
			return -1;
		}
	}

	if( strncmp( buffer, "GET", 3) == 0 )
	{
		//writedata( 1, buffer, i_bytes);
		char * get = new char[i_bytes];
		memcpy( get, buffer, i_bytes);
		io_msg->setData( i_bytes, get, af::Msg::THTTPGET);
		delete []  get;

		//printf("i_bytes = %d, msg data len = %d\n", i_bytes, io_msg->dataLen());
		return 0; // no offset, reading finished
	}

	io_msg->readHeader( i_bytes);

	return af::Msg::SizeHeader;
}

af::Msg * msgsendtoaddress( const af::Msg * i_msg, const af::Address & i_address,
						    bool & o_ok, af::VerboseMode i_verbose)
{
	o_ok = true;

	if( i_address.isEmpty() )
	{
		AFERROR("msgsendtoaddress: Address is empty.")
		o_ok = false;
		return NULL;
	}

	int socketfd;
	struct sockaddr_storage client_addr;

	if( false == i_address.setSocketAddress( &client_addr)) return NULL;

	if(( socketfd = socket( client_addr.ss_family, SOCK_STREAM, 0)) < 0 )
	{
		AFERRPE("msgsendtoaddress: socket() call failed")
		o_ok = false;
		return NULL;
	}

	AFINFO("msgsendtoaddress: tying to connect to client.")
/*
	// Use SIGALRM to unblock
#ifndef WINNT
	if( alarm(2) != 0 )
		AFERROR("af::msgsend: alarm was already set.\n");
#endif //WINNT
*/

	//
	// set socket maximum time to wait for an output operation to complete
	timeval so_timeo;
	so_timeo.tv_usec = 0;
	so_timeo.tv_sec = af::Environment::getServer_SO_MSGTIMEO_SEC();
	if( setsockopt( socketfd, SOL_SOCKET, SO_RCVTIMEO, WINNT_TOCHAR(&so_timeo), sizeof(so_timeo)) != 0)
	{
		AFERRPE("msgsendtoaddress: set socket SO_RCVTIMEO option failed")
		i_address.v_stdOut(); printf("\n");
	}
	if( setsockopt( socketfd, SOL_SOCKET, SO_SNDTIMEO, WINNT_TOCHAR(&so_timeo), sizeof(so_timeo)) != 0)
	{
		AFERRPE("msgsendtoaddress: set socket SO_SNDTIMEO option failed")
		i_address.v_stdOut(); printf("\n");
	}
	int nodelay = 1;
	if( setsockopt( socketfd, IPPROTO_TCP, TCP_NODELAY, WINNT_TOCHAR(&nodelay), sizeof(nodelay)) != 0)
	{
	   AFERRPE("msgsendtoaddress: set socket TCP_NODELAY option failed")
	   i_address.v_stdOut(); printf("\n");
	}

	//
	// connect to address
	if( connect(socketfd, (struct sockaddr*)&client_addr, i_address.sizeofAddr()) != 0 )
	{
		if( i_verbose == af::VerboseOn )
		{
			AFERRPA("msgsendtoaddress: connect failure for msgType '%s':\n%s: ",
				af::Msg::TNAMES[i_msg->type()], i_address.v_generateInfoString().c_str())
		}
		closesocket(socketfd);
/*
#ifndef WINNT
		alarm(0);
#endif //WINNT
*/
		o_ok = false;
		return NULL;
	}
/*
#ifndef WINNT
	alarm(0);
#endif //WINNT
*/
	//
	// send
	if( false == af::msgwrite( socketfd, i_msg))
	{
		AFERRAR("af::msgsend: can't send message to client: %s",
				i_address.v_generateInfoString().c_str())
		closesocket(socketfd);
		o_ok = false;
		return NULL;
	}

	if( false == i_msg->isReceiving())
	{
		closesocket(socketfd);
		return NULL;
	}


	// Read JSON answer:
	if( i_msg->type() == af::Msg::TJSON )
	{
		static const int read_buf_len = 4096;
		char read_buf[read_buf_len];
		std::string buffer;
		while( buffer.size() <= af::Msg::SizeDataMax )
		{
			#ifdef WINNT
			int r = recv( socketfd, read_buf, read_buf_len, 0);
			#else
			int r = read( socketfd, read_buf, read_buf_len);
			#endif
			if( r <= 0 )
				break;
			buffer += std::string( read_buf, r);
		}

		af::Msg * o_msg = NULL;
		if( buffer.size())
		{
			o_msg = new af::Msg();
			o_msg->setData( buffer.size(), buffer.c_str(), af::Msg::TJSON);
			o_ok = true;
		}
		else
		{
			AFERROR("msgsendtoaddress: Reading JSON answer failed.")
			o_ok = false;
		}

		closesocket(socketfd);
		return o_msg;
	}

	//
	// Read binary answer:
	af::Msg * o_msg = new af::Msg();
	if( false == af::msgread( socketfd, o_msg))
	{
	   AFERROR("msgsendtoaddress: Reading binary answer failed.")
	   closesocket( socketfd);
	   delete o_msg;
	   o_ok = false;
	   return NULL;
	}

	closesocket(socketfd);
	return o_msg;
}

void af::statwrite( af::Msg * msg)
{
   mgstat.writeStat( msg);
}
void af::statread( af::Msg * msg)
{
   mgstat.readStat( msg);
}

void af::statout( int columns, int sorting)
{
   mgstat.v_stdOut( columns, sorting);
}

const af::Address af::solveNetName( const std::string & i_name, int i_port, int i_type, VerboseMode i_verbose)
{
	if( i_verbose == af::VerboseOn )
	{
		printf("Solving '%s'", i_name.c_str());
		switch( i_type)
		{
			case AF_UNSPEC: break;
			case AF_INET:  printf(" and IPv4 forced"); break;
			case AF_INET6: printf(" and IPv6 forced"); break;
			default: printf(" (unknown protocol forced)");
		}
		printf("...\n");
	}

	struct addrinfo *res;
	struct addrinfo hints;
	memset( &hints, 0, sizeof(hints));
	hints.ai_flags = AI_ADDRCONFIG;
 //   hints.ai_family = AF_UNSPEC; // This is value is default
	hints.ai_socktype = SOCK_STREAM;
	char service_port[16];
	sprintf( service_port, "%u", i_port);
	int err = getaddrinfo( i_name.c_str(), service_port, &hints, &res);
	if( err != 0 )
	{
		AFERRAR("af::solveNetName:\n%s", gai_strerror(err))
		return af::Address();
	}

	for( struct addrinfo *r = res; r != NULL; r = r->ai_next)
	{
		// Skip address if type is forced
		if(( i_type != AF_UNSPEC ) && ( i_type != r->ai_family)) continue;

		if( i_verbose == af::VerboseOn )
		{
			switch( r->ai_family)
			{
				case AF_INET:
				{
					struct sockaddr_in * sa = (struct sockaddr_in*)(r->ai_addr);
					printf("IP = %s\n", inet_ntoa( sa->sin_addr));
					break;
				}
				case AF_INET6:
				{
					static const int buffer_len = 256;
					char buffer[buffer_len];
					struct sockaddr_in6 * sa = (struct sockaddr_in6*)(r->ai_addr);
					const char * addr_str = inet_ntop( AF_INET6, &(sa->sin6_addr), buffer, buffer_len);
					printf("IPv6 = %s\n", addr_str);
					break;
				}
				default:
					printf("Unknown address family type = %d\n", r->ai_family);
					continue;
			}
		}

		af::Address addr((struct sockaddr_storage*)(r->ai_addr));

		if( i_verbose == af::VerboseOn )
		{
			printf("Address = ");
			addr.v_stdOut();
		}

		// Free memory allocated for addresses:
		freeaddrinfo( res);

		return addr;

	}

	// Free memory allocated for addresses:
	freeaddrinfo( res);

	return af::Address();
}

bool af::msgread( int desc, af::Msg* msg)
{
AFINFO("af::msgread:\n");

	char * buffer = msg->buffer();
	//
	// Read message header data
	int bytes = ::readdata( desc, buffer, af::Msg::SizeHeader, af::Msg::SizeBuffer );

	if( bytes < af::Msg::SizeHeader)
	{
		AFERRAR("af::msgread: can't read message header, bytes = %d (< Msg::SizeHeader).", bytes)
		msg->setInvalid();
		return false;
	}

	// Header offset is variable on not binary header (for example HTTP)
	int header_offset = processHeader( msg, bytes, desc);
	if( header_offset < 0)
		return false;

	//
	// Read message data if any
	if( msg->type() >= af::Msg::TDATA)
	{
		buffer = msg->buffer(); // buffer may be changed to fit new size
		bytes -= header_offset;
		int readlen = msg->dataLen() - bytes;
		if( readlen > 0)
		{
//printf("Need to read more %d bytes of data:\n", readlen);
			bytes = ::readdata( desc, buffer + af::Msg::SizeHeader + bytes, readlen, readlen);
			if( bytes < readlen)
			{
				AFERRAR("af::msgread: read message data: ( bytes < readlen : %d < %d)", bytes, readlen)
				msg->setInvalid();
				return false;
			}
		}
	}
//msg->stdOutData();
	mgstat.put( msg->type(), msg->writeSize());

	return true;
}

bool af::msgwrite( int i_desc, const af::Msg * i_msg)
{
	if( i_msg->type() == af::Msg::THTTP )
	{
		::writedata( i_desc, "HTTP/1.1 200 OK\r\n", 17);
		::writedata( i_desc, "Content-Type: application/json\r\n", 32);
//                            1234567890123456789012345678901234567890
//                            0         1         2         3
		::writedata( i_desc, "\r\n", 2);
	}

	if( false == ::writedata( i_desc, i_msg->buffer() + i_msg->getHeaderOffset(), i_msg->writeSize() - i_msg->getHeaderOffset() ))
	{
		AFERROR("af::msgwrite: Error writing message.\n")
		return false;
	}

	mgstat.put( i_msg->type(), i_msg->writeSize());

	return true;
}

af::Msg * af::msgsend( Msg * i_msg, bool & o_ok, VerboseMode i_verbose )
{
	if( i_msg->isReceiving() && ( i_msg->addressesCount() > 0 ))
	{
		AFERROR("af::msgsend: Receiving message has several addresses.")
	}

	if( i_msg->addressIsEmpty() && ( i_msg->addressesCount() == 0 ))
	{
		AFERROR("af::msgsend: Message has no addresses to send to.")
		o_ok = false;
		i_msg->v_stdOut();
		return NULL;
	}

	if( false == i_msg->addressIsEmpty())
	{
		af::Msg * o_msg = ::msgsendtoaddress( i_msg, i_msg->getAddress(), o_ok, i_verbose);
		if( o_msg != NULL )
			return o_msg;
	}

	if( i_msg->addressesCount() < 1)
		return NULL;

	bool ok;
	const std::list<af::Address> * addresses = i_msg->getAddresses();
	std::list<af::Address>::const_iterator it = addresses->begin();
	std::list<af::Address>::const_iterator it_end = addresses->end();
	while( it != it_end)
	{
		::msgsendtoaddress( i_msg, *it, ok, i_verbose);
		if( false == ok )
		{
			o_ok = false;
			// Store an address that message was failed to send to
			i_msg->setAddress( *it);
		}
		it++;
	}

	return NULL;
}

af::Msg * af::msgString( const std::string & i_str)
{
	af::Msg * o_msg = new af::Msg();
	o_msg->setString( i_str);
	return o_msg;
}

