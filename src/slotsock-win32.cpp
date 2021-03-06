#include <stdio.h>
#include <assert.h>
#include <winsock2.h>

#include "wait/platform.h"
#include "wait/module.h"
#include "wait/slotwait.h"
#include "wait/slotsock.h"

#define MAX_CONN 1000
#define SF_FIRST_IN   1
#define SF_FIRST_OUT  2
#define SF_READY_IN   4
#define SF_READY_OUT  8
#define SF_CLOSE_IN   16
#define SF_CLOSE_OUT  32
#define SF_MESSAGE    64

#define SOCK_MAGIC 0x19821133
#define slotwait_handle() _wait_event

struct sockcb {
	struct sockcb *s_next;

	int s_file;
	int s_flags;
	int s_magic;
	slotcb s_rslot;
	slotcb s_wslot;
};

static int _sock_ref = 0;
static HANDLE _wait_event = 0;
struct sockcb _sockcb_list[MAX_CONN] = {0};
static int (* register_sockmsg)(int fd, int index, int flags) = 0;

struct sockcb *_sockcb_free = 0;
struct sockcb *_sockcb_active = 0;

struct sockcb *sock_attach(int sockfd)
{
	DWORD all_events;
   	struct sockcb *sockcbp;

	all_events = FD_CONNECT| FD_ACCEPT| FD_READ| FD_WRITE| FD_CLOSE;

	sockcbp = _sockcb_free;
	assert(sockcbp != NULL);
	_sockcb_free = _sockcb_free->s_next;

	sockcbp->s_file = sockfd;
	sockcbp->s_magic = SOCK_MAGIC;
	sockcbp->s_rslot = 0;
	sockcbp->s_wslot = 0;
	sockcbp->s_flags = SF_FIRST_IN| SF_FIRST_OUT;

	WSAEventSelect(sockfd, slotwait_handle(), all_events);

	sockcbp->s_next = _sockcb_active;
	_sockcb_active = sockcbp;
	_sock_ref++;

	return sockcbp;
}

int sock_detach(struct sockcb *detachp)
{
	int s_flags;
	int sockfd = -1;
	struct sockcb *sockcbp;
	struct sockcb **sockcbpp;
	assert(detachp->s_magic == SOCK_MAGIC);

	sockcbpp = &_sockcb_active;
	for (sockcbp = _sockcb_active; 
			sockcbp != NULL; sockcbp = sockcbp->s_next) {
		if (detachp == sockcbp) {
			*sockcbpp = sockcbp->s_next;
			sockfd = sockcbp->s_file;
			s_flags = sockcbp->s_flags;
			sockcbp->s_file = -1;

			sockcbp->s_next = _sockcb_free;
			_sockcb_free = sockcbp;
			break;
		}

		sockcbpp = &sockcbp->s_next;
	}

	do {
		assert(sockfd != -1);
		if (s_flags & SF_MESSAGE) {
			if (register_sockmsg) 
				register_sockmsg(sockfd, 0, 0);
			break;
		}
		WSAEventSelect(sockfd, slotwait_handle(), 0);
	} while (0);
	_sock_ref--;

	return sockfd;
}

int getaddrbyname(const char *name, struct sockaddr_in *addr)
{
	char buf[1024];
	in_addr in_addr1;
	u_long peer_addr;
	char *port, *hostname;
	struct hostent *phost;
	struct sockaddr_in *p_addr;

	strcpy(buf, name);
	hostname = buf;

	port = strchr(buf, ':');
	if (port != NULL) {
		*port++ = 0;
	}

	p_addr = (struct sockaddr_in *)addr;
	p_addr->sin_family = AF_INET;
	p_addr->sin_port   = htons(port? atoi(port): 3478);

	peer_addr = inet_addr(hostname);
	if (peer_addr != INADDR_ANY &&
		peer_addr != INADDR_NONE) {
		p_addr->sin_addr.s_addr = peer_addr;
		return 0;
	}

	phost = gethostbyname(hostname);
	if (phost == NULL) {
		return -1;
	}

	memcpy(&in_addr1, phost->h_addr, sizeof(in_addr1));
	p_addr->sin_addr = in_addr1;
	return 0;
}

int get_addr_by_name(const char *name, struct in_addr *ipaddr)
{
	struct hostent *host;
	host = gethostbyname(name);
	if (host == NULL)
		return -1;

	memcpy(ipaddr, host->h_addr_list[0], sizeof(*ipaddr));
	return 0;
}

static void do_quick_scan(void *upp)
{
	int error;
	long events;
   	long io_error;
	struct sockcb *sockcbp;
	WSANETWORKEVENTS network_events;

	DWORD waitstat = WaitForSingleObject(slotwait_handle(), slot_isbusy()? 0: 20);
	if (waitstat == WAIT_OBJECT_0)
		ResetEvent(slotwait_handle());
	assert(waitstat == WAIT_TIMEOUT || waitstat == WAIT_OBJECT_0);

	for (sockcbp = _sockcb_active; 
			sockcbp != NULL; sockcbp = sockcbp->s_next) {
		error = WSAEnumNetworkEvents(sockcbp->s_file, 0, &network_events);
		if (error != 0)
			continue;

		io_error = 0;
		events = network_events.lNetworkEvents;
		if (FD_READ & events) {
			io_error |= network_events.iErrorCode[FD_READ_BIT];
			sockcbp->s_flags |= SF_READY_IN;
			slot_wakeup(&sockcbp->s_rslot);
		}

		if (FD_WRITE & events) {
			io_error |= network_events.iErrorCode[FD_WRITE_BIT];
			sockcbp->s_flags |= SF_READY_OUT;
			slot_wakeup(&sockcbp->s_wslot);
		}

		if (FD_CLOSE & events) {
			io_error |= network_events.iErrorCode[FD_CLOSE_BIT];
			sockcbp->s_flags |= SF_READY_IN;
			sockcbp->s_flags |= SF_CLOSE_IN;
			slot_wakeup(&sockcbp->s_rslot);
		}

		if (FD_ACCEPT & events) {
			io_error |= network_events.iErrorCode[FD_ACCEPT_BIT];
			sockcbp->s_flags |= SF_READY_IN;
			slot_wakeup(&sockcbp->s_rslot);
		}

		if (FD_CONNECT & events) {
			io_error |= network_events.iErrorCode[FD_CONNECT_BIT];
			sockcbp->s_flags |= SF_READY_OUT;
			slot_wakeup(&sockcbp->s_wslot);
		}

		if (io_error != 0) {
			sockcbp->s_flags |= SF_CLOSE_IN;
			sockcbp->s_flags |= SF_CLOSE_OUT;
			slot_wakeup(&sockcbp->s_rslot);
			slot_wakeup(&sockcbp->s_wslot);
		}
	}
}

int sock_read_wait(struct sockcb *sockcbp, struct waitcb *waitcbp)
{
	int flags;
	assert(sockcbp->s_magic == SOCK_MAGIC);

	if (waitcb_active(waitcbp) &&
		   	waitcbp->wt_data == &sockcbp->s_rslot)
		return 0;

	slot_record(&sockcbp->s_rslot, waitcbp);
	flags = sockcbp->s_flags & (SF_READY_IN| SF_FIRST_IN);

	sockcbp->s_flags &= ~SF_FIRST_IN;
	if (flags == (SF_READY_IN| SF_FIRST_IN)) {
		slot_wakeup(&sockcbp->s_rslot);
		return 0;
	}

	if (sockcbp->s_flags & SF_CLOSE_IN) {
		slot_wakeup(&sockcbp->s_rslot);
		return 0;
	}
	
	waitcbp->wt_data = &sockcbp->s_rslot;
	return 0;
}

int sock_write_wait(struct sockcb *sockcbp, struct waitcb *waitcbp)
{
	int flags;
	assert(sockcbp->s_magic == SOCK_MAGIC);

	if (waitcb_active(waitcbp) &&
			waitcbp->wt_data == &sockcbp->s_wslot)
		return 0;

	slot_record(&sockcbp->s_wslot, waitcbp);
	flags = sockcbp->s_flags & (SF_READY_OUT| SF_FIRST_OUT);

	sockcbp->s_flags &= ~SF_FIRST_OUT;
	if (flags == (SF_READY_OUT| SF_FIRST_OUT)) {
		slot_wakeup(&sockcbp->s_wslot);
		return 0;
	}

	if (sockcbp->s_flags & SF_CLOSE_OUT) {
		slot_wakeup(&sockcbp->s_wslot);
		return 0;
	}
	
	waitcbp->wt_data = &sockcbp->s_wslot;
	return 0;
}

int sock_wakeup(int sockfd, int index, int type)
{
	struct sockcb *sockcbp;

	if (0 <= index && index < MAX_CONN) {
		sockcbp = &_sockcb_list[index];
		if (sockcbp->s_file != sockfd) {
			fprintf(stderr, "call sock_wakeup falure\n");
			return 0;
		}

		switch (type) {
			case FD_READ:
			case FD_CLOSE:
			case FD_ACCEPT:
				sockcbp->s_flags |= SF_READY_IN;
			   	slot_wakeup(&sockcbp->s_rslot);
				break;

			case FD_WRITE:
			case FD_CONNECT:
				sockcbp->s_flags |= SF_READY_OUT;
			   	slot_wakeup(&sockcbp->s_wslot);
				break;

			case FD_OOB:
			default:
				sockcbp->s_flags |= SF_CLOSE_OUT;
				sockcbp->s_flags |= SF_CLOSE_IN;
			   	slot_wakeup(&sockcbp->s_wslot);
			   	slot_wakeup(&sockcbp->s_rslot);
				break;
		}
	}

	return 0;
}

static struct waitcb _sock_waitcb;
static void module_init(void)
{
	int i;
	WSADATA data;
	struct sockcb *sockcbp;

	_sockcb_free = 0;
	_sockcb_active = 0;
	for (i = 0; i < MAX_CONN; i++) {
		sockcbp = &_sockcb_list[i];
		sockcbp->s_next = _sockcb_free;
		_sockcb_free = sockcbp;
	}
   
	waitcb_init(&_sock_waitcb, do_quick_scan, 0);
	_sock_waitcb.wt_flags &= ~WT_EXTERNAL;
	_sock_waitcb.wt_flags |= WT_WAITSCAN;
	waitcb_switch(&_sock_waitcb);

	_wait_event = CreateEvent(NULL, FALSE, FALSE, 0);
	WSAStartup(0x101, &data);
}

static void module_clean(void)
{
	CloseHandle(_wait_event);
	WSACleanup();
}

struct module_stub slotsock_mod = {
	module_init, module_clean
};


static int blocking(int fd)
{
	return (WSAGetLastError() == WSAEWOULDBLOCK);
}

static int op_read(int fd, void *buf, int len)
{
	return recv(fd, (char *)buf, len, 0);
}

static int op_write(int fd, void *buf, int len)
{
	return send(fd, (char *)buf, len, 0);
}

static void read_wait(void *upp, struct waitcb *wait)
{
	sock_read_wait((struct sockcb *)upp, wait);
}

static void write_wait(void *upp, struct waitcb *wait)
{
	sock_write_wait((struct sockcb *)upp, wait);
}

static void do_shutdown(int file, int cond)
{
	if (cond != 0)
		shutdown(file, SHUT_WR);
	return;
}

struct sockop winsock_ops = {
	blocking, op_read, op_write, read_wait, write_wait, do_shutdown
};

