/* Author: newsun
 * Date: 2016.1
 * ns network library
 * used for network packet transfer more easily
 * All rights reserved
 * ver 0.8
 * email: 30037494@qq.com
 */

#ifndef NS_NETLIB_H
#define NS_NETLIB_H

#include <pthread.h>
#include <sys/time.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int 		magic; //magic number to verify the packet
	int 		type; //the type of packet
	int		len; //indicate the len of para
	int 		reserve1;
	unsigned int 	reserve2;
	char		para[0]; //parameter extended
} ns_header_t;

typedef struct {
	char		*IP;
	short		port;
	int		reserve1;
	unsigned int	reserve2;
} ns_reg_t;

typedef struct {
        pthread_mutex_t         lock;
        pthread_cond_t          cond;
        int                     count;
} cond_t;

typedef struct {
        int                     packageSize;
        int                     packageCount;
        void                    *pBuff;
        pthread_mutex_t         lock;
        cond_t                  cond;
        int                     read;
        int                     write;
        int                     threadFlag;
} buff_t;

typedef struct {
        int                     fd;
	int			ns_errno;
        buff_t                  *commBuff;
	buff_t			*HBBuff;
	int			*DPthreadFlag;
	time_t			*timeStamp;
} ns_fd_t;


ns_fd_t ns_client_init(ns_reg_t *ns_reg); //return ns_fd

//ns_fd_t ns_server_init(ns_reg_t *ns_reg); //return ns_fd

int ns_read(ns_fd_t *ns_fd, void *buff, int len); //return -1 while error occurs, and then check lasterror;

int ns_write(ns_fd_t *ns_fd, const void *buff, int len);

int ns_lasterror(ns_fd_t *ns_fd);

int ns_fini(ns_fd_t *ns_fd);

#ifdef __cplusplus
}
#endif

#endif //NS_NETLIB_H

