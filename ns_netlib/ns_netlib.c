/* Author: newsun
 * Date: 2016.1
 * ns network library
 * used for network packet transfer more easily
 * All rights reserved
 * ver 0.8
 * email: 30037494@qq.com
 */

#include "ns_netlib.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <sys/timerfd.h>
#include <sys/select.h>
#include <sys/time.h>
#include <signal.h>


#ifdef __cplusplus
extern "C" {
#endif

#define NEWSUN_NORMAL		0	/*newsun:normal*/
#define NEWSUN_EPARA		1	/*newsun:wrong parameter*/
#define NEWSUN_ELEVEL		2	/*newsun:wrong log level*/
#define NEWSUN_ENETWORK		3	/*newsun:network problem*/
#define NEWSUN_ETHREAD		4	/*newsun:something wrong with thread*/
#define NEWSUN_ETIMER		5	/*newsun:something wrong with timer*/
#define NEWSUN_ESELECT		6
#define NEWSUN_EWRITE		7
#define NEWSUN_EREAD		8
#define NEWSUN_EHEADER		9

#define LOGMODE_FILE	0x0001
#define LOGMODE_STD	0x0002
#define LOGBUFF_MAX	4096
#define LOGLEVEL_ERROR	1
#define LOGLEVEL_INFO	2
#define LOGLEVEL_DEBUG	3

#define BUFFPACKAGE_MAX	100	/*newsun:maximum number of packages in buffer*/
#define PACKAGE_PARA_MAX 1024	/*newsun:modify this to adapt your application*/

#define HB_INTERVAL	5	/*newsun:modify this to ajust hearbeat interval(second)*/
#define HB_TIMEOUT	5*2	/*newsun:modify this to ajust hearbeat timeout(second)*/

#define MAGICNUM	0x372B4CEF /*newsun:verify package header*/

#define NEWSUN_HB	1	/*newsun:packet type*/
#define NEWSUN_COMM	2

#define max(a,b)	((a)>(b)?(a):(b))


typedef struct {
	ns_header_t		header;
	char			para[PACKAGE_PARA_MAX];
} package_t;

typedef struct {
	int 			fd;
	int			threadFlag;
	buff_t			*commBuff;
	buff_t			*HBBuff;
	time_t			*timeStamp;
} dispatcherCBArg_t;

typedef struct {
	buff_t			*HBBuff;
	time_t			*timeStamp;
} HBCBArg_t;


static FILE 		*_gLogFile = NULL;
pthread_mutex_t 	_gLogLock = PTHREAD_MUTEX_INITIALIZER;
static 			_gLogLevel = 0;
pthread_mutex_t		_gLogLevelLock = PTHREAD_MUTEX_INITIALIZER; 


static int log_printf(short mode, int level, const char *format, ...) {
	struct timeval	tv;
	struct tm	*tm;
	va_list		va;
	char		buff[LOGBUFF_MAX + 1];
	char		*strLogLevel;
	
	if (!format) {
		return -1;
	}

	if (level > _gLogLevel) {
		return -1;
	}

	va_start(va, format);
	vsnprintf(buff, LOGBUFF_MAX, format, va);
	va_end(va);

	switch(level) {
	case 1: 
		strLogLevel = "[Error]";
		break;
	case 2:
		strLogLevel = "[Info]";
		break;
	case 3:
		strLogLevel = "[Debug]";
		break;
	default:
		strLogLevel = "[]";
	}

	pthread_mutex_lock(&_gLogLock);
	if (_gLogFile == NULL) {
		_gLogFile = fopen("ns_netlib_log.txt", "a");
	}

	switch(mode) {
		case LOGMODE_FILE:
			gettimeofday(&tv, NULL);
			tm = localtime(&(tv.tv_sec));
			
			fprintf(_gLogFile, "%s[%02d:%02d:%02d:%03d:%03d]%s",
				strLogLevel, 
				(int)(tm->tm_hour),
				(int)(tm->tm_min),
				(int)(tm->tm_sec),
				(int)(tv.tv_usec/1000),
				(int)(tv.tv_usec%1000),
				buff);
			fflush(_gLogFile);
			break;

		case LOGMODE_STD://tbd
			break;
		default:
			break;
	}//switch
	pthread_mutex_unlock(&_gLogLock);
	
	return 0;
}

#define log_error(format, ...) \
do { \
	log_printf(LOGMODE_FILE, LOGLEVEL_ERROR, format, ##__VA_ARGS__); \
} while(0)

#define log_info(format, ...) \
do { \
	log_printf(LOGMODE_FILE, LOGLEVEL_INFO, format, ##__VA_ARGS__); \
} while(0)

#define log_debug(format, ...) \
do { \
	log_printf(LOGMODE_FILE, LOGLEVEL_DEBUG, format, ##__VA_ARGS__); \
} while(0)

void ns_setLogLevel(int level) {
	pthread_mutex_lock(&_gLogLevelLock);
	_gLogLevel = level;
	pthread_mutex_unlock(&_gLogLevelLock);

	return;
}

static int newsun_read(int fd, void *pBuf, int len) {
	int	left = len;
	int	n;
	char	*p = (char *)pBuf;

	if ( (fd < 0) || (!pBuf) || (len <0) ) {
		log_error("newsun_read:invalid parameter\n");
		return -1;
	}

	while (left > 0) {
again:
		n = read(fd, p, left); /*newsun:may be blocked here, can use NB-IO, tbd*/
		if (n < 0) {
			if ((errno==EINTR) || (errno==EWOULDBLOCK) || (errno==EAGAIN)) {
				goto again;
			}
			log_error("newsun_read:read\n");
			return -1;
		}
		else if (n == 0) {
			return  (len - left);
		}

		left -= len;
		p += len;
	}//while

	return (len - left);
}

static int newsun_write(int fd, const void *pBuf, int len) {
	int 	left = len;
	int 	n;
	char 	*p = (char *)pBuf;

	if ( (fd < 0) || (!pBuf) || (len <0) ) {
		log_error("newsun_write:invalid parameter\n");
		return -1;
	}

	while (left > 0) {
again:
		n = write(fd, p, left); 
		if (n < 0) {
			if ((errno==EINTR) || (errno==EWOULDBLOCK) || (errno==EAGAIN)) {
				goto again;
			}
			log_error("newsun_write: write\n");
			return -1;
		}
		left -= n;
		p += n;
	}
	
	return (len - left);		
}

static void *newsun_dispatcherCB(void *arg) {
	ns_fd_t 		*ns_fd = (ns_fd_t *)arg;
	int			fd = ns_fd->fd;
	buff_t			*commBuff = ns_fd->commBuff;
	buff_t			*HBBuff = ns_fd->HBBuff;
	time_t			*timeStamp = ns_fd->timeStamp; /*may be thread specific data*/
	int 			timerFd;
	struct itimerspec 	it;
	fd_set			rset;
	int			maxFd;
	ns_header_t		ns_header;
	int			readLen;
	char			buff[PACKAGE_PARA_MAX+sizeof(ns_header)];
	int			paraLen;
	package_t		package;
	ns_header_t		*pHeader;

	if (fd < 0) {
		log_error("dispatcherCB:fd\n");
		return NULL;
	}
	
	if ( (timerFd = timerfd_create(CLOCK_REALTIME, 0)) < 0 ) {
		log_error("dispatcherCB:create timer\n");
		return NULL;
	}
	bzero(&it, sizeof(struct itimerspec));
	it.it_value.tv_sec = HB_INTERVAL;
	it.it_value.tv_nsec = 0;
	it.it_interval.tv_sec = HB_INTERVAL;
	it.it_interval.tv_nsec = 0;
	if ( timerfd_settime(timerFd, 0, &it, NULL) < 0 ) {
		log_error("dispatcherCB:set timer\n");
		return NULL;
	} 
	
	*timeStamp = time(NULL);
	
	FD_ZERO(&rset);
	while (*(ns_fd->DPthreadFlag)) {
		FD_SET(timerFd, &rset);
		FD_SET(fd, &rset);
		maxFd = max(timerFd, fd) + 1;
		if ( select(maxFd, &rset, NULL, NULL, NULL) < 0 ) {
			log_error("dispatcherCB:select\n");
			return NULL;
		}	

		if (FD_ISSET(timerFd, &rset)) {
			if ( ((int)time(NULL) - (int)*timeStamp) > HB_TIMEOUT ) {
				/*newsun:HB lost handler, tbd*/	
			}

			/*newsun:send HB*/
			bzero(&ns_header, sizeof(ns_header));
			ns_header.magic = MAGICNUM;
			ns_header.type = NEWSUN_HB;
			ns_header.len = 0;
			ns_header.reserve1 = (int)time(NULL);
			newsun_write(fd, &ns_header, sizeof(ns_header));
		}

		if (FD_ISSET(fd, &rset)) {
			readLen = newsun_read(fd, buff, sizeof(ns_header));
			if (readLen == 0) {
				log_error("server terminated unexpectedly\n");
				//tbd
			}
			if (readLen != sizeof(ns_header)) {
				log_error("dispatcherCB:newsun_read\n");
				continue;
			}
			if (((ns_header_t *)buff)->magic != MAGICNUM) {
				log_error("dispatcherCB:wrong header\n");
				continue;
			}
		
			paraLen = ((ns_header_t *)buff)->len;
			readLen = newsun_read(fd, buff+sizeof(ns_header), paraLen);
			if (readLen != sizeof(paraLen)) {
				log_error("dispatcherCB:newsun_read\n");
				continue;
			}

			pHeader = (ns_header_t *)buff;
			package.header = *pHeader;
			memcpy(package.para, pHeader->para, pHeader->len);
			
			/*newsun:dispatch to buffer according to package type*/
			if (package.header.type == NEWSUN_HB) {
				if ( ((HBBuff->write+1)%HBBuff->packageCount) != HBBuff->read ) {
					memcpy( (char *)(HBBuff->pBuff) + HBBuff->write*HBBuff->packageSize, &package, sizeof(package));	
					HBBuff->write = (HBBuff->write+1)%HBBuff->packageCount;
				}
				else {
					log_error("dispatcherCB:HBBuff is full\n");
					continue;
				}
				pthread_mutex_lock(&(HBBuff->cond.lock));
				HBBuff->cond.count = 1;
				pthread_cond_signal(&(HBBuff->cond.cond));
				pthread_mutex_unlock(&(HBBuff->cond.lock));
												
			}
			else if (package.header.type == NEWSUN_COMM) {
				if ( ((commBuff->write+1)%commBuff->packageCount) != commBuff->read ) {
					memcpy( (char *)(commBuff->pBuff) + commBuff->write*commBuff->packageSize, &package, sizeof(package));	
					commBuff->write = (commBuff->write+1)%commBuff->packageCount;
				}
				else {
					log_error("dispatcherCB:commBuff is full\n");
					continue;
				}
				pthread_mutex_lock(&(commBuff->cond.lock));
				commBuff->cond.count = 1;
				pthread_cond_signal(&(commBuff->cond.cond));
				pthread_mutex_unlock(&(commBuff->cond.lock));
			}
		}
	} //while

	bzero(&it, sizeof(struct itimerspec));
	if ( timerfd_settime(timerFd, 0, &it, NULL) < 0 ) {
		log_error("dispatcherCB:disarm timer\n");
		return NULL;
	} 
		
	return NULL;	
}

static void *newsun_HBCB(void *arg) {
	ns_fd_t	*ns_fd = (ns_fd_t *)arg;
	buff_t		*HBBuff = ns_fd->HBBuff;
	time_t		*timeStamp = ns_fd->timeStamp;
	int		count, i;
	package_t	*package;

	while (HBBuff->threadFlag) {
		pthread_mutex_lock(&(HBBuff->cond.lock));
		while (HBBuff->cond.count == 0) {
			pthread_cond_wait(&(HBBuff->cond.cond), &(HBBuff->cond.lock));
		}
		HBBuff->cond.count = 0;
		pthread_mutex_unlock(&(HBBuff->cond.lock));

		/*newsun:read package*/
		if (HBBuff->read != HBBuff->write) {
			count = (HBBuff->write + HBBuff->packageCount - HBBuff->read) % HBBuff->packageCount;
			for (i = 0; i< count; i++) {
				package = (package_t *)((char *)HBBuff->pBuff + HBBuff->read * HBBuff->packageSize);	
				HBBuff->read = (HBBuff->read + 1) % HBBuff->packageCount;
				if (package->header.type == NEWSUN_HB) {
					*timeStamp = time(NULL);
					log_debug("newsun_HBCB:receiveHB, timeStamp:%d\n", (int)*timeStamp);
				}
			}
		}
	}//while

	return NULL;
}

static void newsun_initBuff(buff_t *pBuff) {
	pBuff->packageSize = sizeof(package_t);
	pBuff->packageCount = BUFFPACKAGE_MAX;
	pBuff->pBuff = calloc(pBuff->packageCount, pBuff->packageSize); /*to be free*/
	//pBuff->lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_init(&(pBuff->lock), NULL);
	
	pBuff->cond.count = 0;
	//pBuff->cond.lock = PTHREAD_MUTEX_INITIALIZER;
	pthread_mutex_init(&(pBuff->cond.lock), NULL);
	pthread_cond_init(&(pBuff->cond.cond), NULL);

	pBuff->read= pBuff->write = 0;

	pBuff->threadFlag = 1; //newsun:enable thread running

	return;	
}

ns_fd_t ns_client_init(ns_reg_t *ns_reg) {
	int 			fd;
	struct sockaddr_in	addr;
	pthread_t		dispatcherCB_pid, HBCB_pid;
	buff_t			*commBuff, *HBBuff; /*may be thread specific data*/
	time_t			*timeStamp; /*may be thread specific data*/
	ns_fd_t			ns_fd;
	int			*DPthreadFlag;
	
	ns_fd.ns_errno = NEWSUN_NORMAL;

	if (fd = socket(AF_INET, SOCK_STREAM, 0) < 0) {
		ns_fd.ns_errno = NEWSUN_ENETWORK;
		log_error("ns_client_init:socket\n");
		return ns_fd;
	}

	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(ns_reg->port);
	if (inet_pton(AF_INET, ns_reg->IP, &addr.sin_addr) < 0) {
		ns_fd.ns_errno = NEWSUN_ENETWORK;
		log_error("ns_client_init:inet_pton\n");
		return ns_fd;
	}		
	
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		ns_fd.ns_errno = NEWSUN_ENETWORK;
		log_error("ns_client_init:connect\n");
		return ns_fd;
	}

	commBuff = (buff_t *)malloc(sizeof(buff_t)); /*to be free*/
	HBBuff = (buff_t *)malloc(sizeof(buff_t)); /*to be free*/
	timeStamp = (time_t *)malloc(sizeof(time_t)); /*to be free*/
	DPthreadFlag = (int *)malloc(sizeof(int)); /*to be free*/
	
	newsun_initBuff(commBuff);
	newsun_initBuff(HBBuff);
	
	ns_fd.fd = fd;
	ns_fd.commBuff = commBuff;
	ns_fd.HBBuff = HBBuff;
	ns_fd.timeStamp = timeStamp;
	ns_fd.DPthreadFlag = DPthreadFlag;

	*(ns_fd.DPthreadFlag) = 1;
	if (pthread_create(&dispatcherCB_pid, NULL, newsun_dispatcherCB, (void *)&ns_fd) != 0) {
		ns_fd.ns_errno = NEWSUN_ETHREAD;
		log_error("ns_client_init:create thread\n");
		return ns_fd;
	}

	if (pthread_create(&HBCB_pid, NULL, newsun_HBCB, (void *)&ns_fd) != 0) {
		ns_fd.ns_errno = NEWSUN_ETHREAD;
		log_error("ns_client_init:create thread\n");
		return ns_fd;
	}
	
	return ns_fd;
}

static void *newsun_connCB(void *arg) {
	ns_fd_t 		*ns_fd = (ns_fd_t *)arg;
	int			fd = ns_fd->fd;
	buff_t			*commBuff = ns_fd->commBuff;
	buff_t			*HBBuff = ns_fd->HBBuff;
	time_t			*timeStamp = ns_fd->timeStamp; /*may be thread specific data*/
	int 			timerFd;
	struct itimerspec 	it;
	fd_set			rset;
	int			maxFd;
	ns_header_t		ns_header;
	int			readLen;
	char			buff[PACKAGE_PARA_MAX+sizeof(ns_header)];
	int			paraLen;
	package_t		package;
	ns_header_t		*pHeader;
	int			threadFlag = 1;

	
	if ( (timerFd = timerfd_create(CLOCK_REALTIME, 0)) < 0 ) {
		log_error("newsun_connCB:create timer\n");
		return NULL;
	}
	bzero(&it, sizeof(struct itimerspec));
	it.it_value.tv_sec = HB_INTERVAL;
	it.it_value.tv_nsec = 0;
	it.it_interval.tv_sec = HB_INTERVAL;
	it.it_interval.tv_nsec = 0;
	if ( timerfd_settime(timerFd, 0, &it, NULL) < 0 ) {
		log_error("newsun_connCB:set timer\n");
		return NULL;
	} 
	
	*timeStamp = time(NULL);
	
	FD_ZERO(&rset);
	while (threadFlag) {
		FD_SET(timerFd, &rset);
		FD_SET(fd, &rset);
		maxFd = max(timerFd, fd) + 1;
		if ( select(maxFd, &rset, NULL, NULL, NULL) < 0 ) {
			log_error("newsun_connCB:select\n");
			return NULL;
		}	

		if (FD_ISSET(timerFd, &rset)) {
			if ( ((int)time(NULL) - (int)*timeStamp) > HB_TIMEOUT ) {
				/*newsun:HB lost handler, tbd*/	
			}

			/*newsun:send HB*/
			bzero(&ns_header, sizeof(ns_header));
			ns_header.magic = MAGICNUM;
			ns_header.type = NEWSUN_HB;
			ns_header.len = 0;
			ns_header.reserve1 = (int)time(NULL);
			newsun_write(fd, &ns_header, sizeof(ns_header));
		}

		if (FD_ISSET(fd, &rset)) {
			readLen = newsun_read(fd, buff, sizeof(ns_header));
			if (readLen == 0) {
				threadFlag = 0;
				continue;
			}
			if (readLen != sizeof(ns_header)) {
				log_error("newsun_connCB:newsun_read\n");
				continue;
			}
			if (((ns_header_t *)buff)->magic != MAGICNUM) {
				log_error("newsun_connCB:wrong header\n");
				continue;
			}
		
			paraLen = ((ns_header_t *)buff)->len;
			readLen = newsun_read(fd, buff+sizeof(ns_header), paraLen);
			if (readLen != sizeof(paraLen)) {
				log_error("newsun_connCB:newsun_read\n");
				continue;
			}

			pHeader = (ns_header_t *)buff;
			package.header = *pHeader;
			memcpy(package.para, pHeader->para, pHeader->len);
			
			/*newsun:dispatch to buffer according to package type*/
			if (package.header.type == NEWSUN_HB) {
				if ( ((HBBuff->write+1)%HBBuff->packageCount) != HBBuff->read ) {
					memcpy( (char *)(HBBuff->pBuff) + HBBuff->write*HBBuff->packageSize, &package, sizeof(package));	
					HBBuff->write = (HBBuff->write+1)%HBBuff->packageCount;
				}
				else {
					log_error("newsun_connCB:HBBuff is full\n");
					continue;
				}
				pthread_mutex_lock(&(HBBuff->cond.lock));
				HBBuff->cond.count = 1;
				pthread_cond_signal(&(HBBuff->cond.cond));
				pthread_mutex_unlock(&(HBBuff->cond.lock));
												
			}
			else if (package.header.type == NEWSUN_COMM) {
				if ( ((commBuff->write+1)%commBuff->packageCount) != commBuff->read ) {
					memcpy( (char *)(commBuff->pBuff) + commBuff->write*commBuff->packageSize, &package, sizeof(package));	
					commBuff->write = (commBuff->write+1)%commBuff->packageCount;
				}
				else {
					log_error("newsun_connCB:commBuff is full\n");
					continue;
				}
				pthread_mutex_lock(&(commBuff->cond.lock));
				commBuff->cond.count = 1;
				pthread_cond_signal(&(commBuff->cond.cond));
				pthread_mutex_unlock(&(commBuff->cond.lock));
			}
		}
	} //while

	bzero(&it, sizeof(struct itimerspec));
	if ( timerfd_settime(timerFd, 0, &it, NULL) < 0 ) {
		log_error("newsun_connCB:disarm timer\n");
		return NULL;
	} 
	close(ns_fd->fd);
	ns_fd->fd = -1;
	
	ns_fd->commBuff->threadFlag = 0;
	pthread_mutex_lock(&(ns_fd->commBuff->cond.lock));
	ns_fd->commBuff->cond.count = 1;
	pthread_cond_signal(&(ns_fd->commBuff->cond.cond));
	pthread_mutex_unlock(&(ns_fd->commBuff->cond.lock));
	sleep(1);
	if(ns_fd->commBuff->pBuff) {
		free(ns_fd->commBuff->pBuff);
		ns_fd->commBuff->pBuff = NULL;
	}

	ns_fd->HBBuff->threadFlag = 0;
	pthread_mutex_lock(&(ns_fd->HBBuff->cond.lock));
	ns_fd->HBBuff->cond.count = 1;
	pthread_cond_signal(&(ns_fd->HBBuff->cond.cond));
	pthread_mutex_unlock(&(ns_fd->HBBuff->cond.lock));
	sleep(1);
	if (ns_fd->HBBuff->pBuff) {
		free(ns_fd->HBBuff->pBuff);
		ns_fd->HBBuff->pBuff = NULL;
	}

	if (ns_fd->commBuff) {
		free(ns_fd->commBuff);
		ns_fd->commBuff = NULL;
	}
	if (ns_fd->HBBuff) {
		free(ns_fd->HBBuff);
		ns_fd->HBBuff = NULL;
	}
	if (ns_fd->timeStamp) {
		free(ns_fd->timeStamp);
		ns_fd->timeStamp = NULL;
	}

	free(ns_fd);
	ns_fd = NULL;
	
	return NULL;	
}

static void *newsun_listenCB(void *arg) {
	int 		fd = (int)arg, connFd;
	pthread_t	connCB_pid, HBCB_pid;
	ns_fd_t		*ns_fd;
	buff_t		*commBuff, *HBBuff;
	time_t		*timeStamp;
	

	for (;;) {
		connFd = accept(fd, NULL, NULL);

		commBuff = (buff_t *)malloc(sizeof(buff_t)); /*to be free*/
		HBBuff = (buff_t *)malloc(sizeof(buff_t)); /*to be free*/
		timeStamp = (time_t *)malloc(sizeof(time_t)); /*to be free*/
		ns_fd = (ns_fd_t *)malloc(sizeof(ns_fd_t)); /*to be free*/

		newsun_initBuff(commBuff);
		newsun_initBuff(HBBuff);
	
		ns_fd->fd = connFd;
		ns_fd->commBuff = commBuff;
		ns_fd->HBBuff = HBBuff;
		ns_fd->timeStamp = timeStamp;

		if (pthread_create(&connCB_pid, NULL, newsun_connCB, (void *)ns_fd) != 0) {
			log_error("newsun_listenCB:create thread\n");
			return ns_fd;
		}

		if (pthread_create(&HBCB_pid, NULL, newsun_HBCB, (void *)ns_fd) != 0) {
			log_error("newsun_listenCB:create thread\n");
			return ns_fd;
		}
	}	
}

ns_fd_t ns_server_init(ns_reg_t *ns_reg) {
	int 			fd;
	struct sockaddr_in	addr;
	pthread_t		listenCB_pid;
	buff_t			*commBuff, *HBBuff; /*may be thread specific data*/
	time_t			*timeStamp; /*may be thread specific data*/
	ns_fd_t			ns_fd;
	int			*DPthreadFlag;
	int			optval;
	
	ns_fd.ns_errno = NEWSUN_NORMAL;

	if (fd = socket(AF_INET, SOCK_STREAM, 0) < 0) {
		ns_fd.ns_errno = NEWSUN_ENETWORK;
		log_error("ns_server_init:socket\n");
		return ns_fd;
	}

	signal(SIGPIPE, SIG_IGN);

	optval = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, (socklen_t)sizeof(optval)) < 0) {
		ns_fd.ns_errno = NEWSUN_ENETWORK;
		log_error("ns_server_init:socket\n");
		return ns_fd;
	}

	bzero(&addr, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(ns_reg->port);
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	
	if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		ns_fd.ns_errno = NEWSUN_ENETWORK;
		log_error("ns_server_init:bind\n");
		return ns_fd;
	}

	if (pthread_create(&listenCB_pid, NULL, newsun_listenCB, (void *)fd) != 0) {
		ns_fd.ns_errno = NEWSUN_ETHREAD;
		log_error("ns_server_init:create thread\n");
		return ns_fd;
	}

	return ns_fd;
}

int ns_read (ns_fd_t *ns_fd, void *buff, int len) {
 	buff_t		*commBuff = ns_fd->commBuff;
	int		fd = ns_fd->fd;
	package_t	*package;
	int		count, i;

	ns_fd->ns_errno = NEWSUN_NORMAL;
	
	if ( (fd < 0) || (!commBuff) ) {
		ns_fd->ns_errno = NEWSUN_EPARA;
		log_error("ns_read:wrong parameter\n");
		return -1;
	}

	pthread_mutex_lock(&(commBuff->cond.lock));
	while (commBuff->cond.count == 0) { //maybe extend to timeout read in future
		pthread_cond_wait(&(commBuff->cond.cond), &(commBuff->cond.lock));
	}
	commBuff->cond.count = 0;
	pthread_mutex_unlock(&(commBuff->cond.lock));
	
	if (commBuff->write != commBuff->read) {
		commBuff->read = (commBuff->read + 1) % commBuff->packageCount;
		package = (package_t *)((char *)commBuff->pBuff + commBuff->read * commBuff->packageSize);
		if (package->header.type == NEWSUN_COMM) {
			memcpy(buff, package, sizeof(package_t));
			return (sizeof(package_t));
		}
		else {
			ns_fd->ns_errno = NEWSUN_EREAD;
			log_error("ns_read:wrong package\n");
			return -1;
		}
	}
	ns_fd->ns_errno = NEWSUN_EREAD;
	log_error("ns_read:commBuff is empty\n");	
	
	return -1;
}

int ns_write(ns_fd_t *ns_fd, const void *buff, int len) {
	int n, fd = ns_fd->fd;
	
	ns_fd->ns_errno = NEWSUN_NORMAL;
	if (fd < 0) {
		ns_fd->ns_errno = NEWSUN_EPARA;
		log_error("ns_write:invalid parameter\n");
		return -1;
	}
	n = newsun_write(fd, buff, len);
	
	return n;
}

int ns_lasterror(ns_fd_t *ns_fd) {
	return ns_fd->ns_errno;
}

int ns_fini(ns_fd_t *ns_fd) {
	close(ns_fd->fd);
	ns_fd->fd = -1;

	*(ns_fd->DPthreadFlag) = 0; //ask thread to exit

	ns_fd->commBuff->threadFlag = 0;
	pthread_mutex_lock(&(ns_fd->commBuff->cond.lock));
	ns_fd->commBuff->cond.count = 1;
	pthread_cond_signal(&(ns_fd->commBuff->cond.cond));
	pthread_mutex_unlock(&(ns_fd->commBuff->cond.lock));
	sleep(2);

	if(ns_fd->commBuff->pBuff) {
		free(ns_fd->commBuff->pBuff);
		ns_fd->commBuff->pBuff = NULL;
	}

	ns_fd->HBBuff->threadFlag = 0;
	pthread_mutex_lock(&(ns_fd->HBBuff->cond.lock));
	ns_fd->HBBuff->cond.count = 1;
	pthread_cond_signal(&(ns_fd->HBBuff->cond.cond));
	pthread_mutex_unlock(&(ns_fd->HBBuff->cond.lock));
	sleep(2);

	if (ns_fd->HBBuff->pBuff) {
		free(ns_fd->HBBuff->pBuff);
		ns_fd->HBBuff->pBuff = NULL;
	}

	if (ns_fd->commBuff) {
		free(ns_fd->commBuff);
		ns_fd->commBuff = NULL;
	}
	
	if (ns_fd->HBBuff) {
		free(ns_fd->HBBuff);
		ns_fd->HBBuff = NULL;
	}

	if (ns_fd->timeStamp) {
		free(ns_fd->timeStamp);
		ns_fd->timeStamp = NULL;
	}

	if (ns_fd->DPthreadFlag) {
		free(ns_fd->DPthreadFlag);
		ns_fd->DPthreadFlag = NULL;
	}

	return 0;
}

#ifdef __cplusplus
}
#endif

