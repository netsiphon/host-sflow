/* This software is distributed under the following license:
 * http://sflow.net/license.html
 */

#if defined(__cplusplus)
extern "C" {
#endif

#include "util_netlink.h"

  /*_________________---------------------------__________________
    _________________    UTNLDiag_sockid_print  __________________
    -----------------___________________________------------------
  */

  char *UTNLDiag_sockid_print(struct inet_diag_sockid *sockid) {
    static char buf[256];
    snprintf(buf, 256, "%08x:%08x:%08x:%08x %u - %08x:%08x:%08x:%08x %u if:%u",
	     sockid->idiag_src[0],
	     sockid->idiag_src[1],
	     sockid->idiag_src[2],
	     sockid->idiag_src[3],
	     ntohs(sockid->idiag_sport),
	     sockid->idiag_dst[0],
	     sockid->idiag_dst[1],
	     sockid->idiag_dst[2],
	     sockid->idiag_dst[3],
	     ntohs(sockid->idiag_dport),
	     sockid->idiag_if);
    return buf;
  }
	     
  /*_________________---------------------------__________________
    _________________      UTNLDiag_send        __________________
    -----------------___________________________------------------
  */

  int UTNLDiag_send(int sockfd, void *req, int req_len, bool dump, uint32_t seqNo) {
    struct nlmsghdr nlh = { 0 };
    nlh.nlmsg_len = NLMSG_LENGTH(req_len);
    nlh.nlmsg_flags = NLM_F_REQUEST;
    if(dump)
      nlh.nlmsg_flags |= NLM_F_DUMP;
    nlh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    nlh.nlmsg_seq = seqNo;

    struct iovec iov[2];
    iov[0].iov_base = (void*) &nlh;
    iov[0].iov_len = sizeof(nlh);
    iov[1].iov_base = req;
    iov[1].iov_len = req_len;
    
    struct sockaddr_nl sa = { 0 };
    sa.nl_family = AF_NETLINK;
    
    struct msghdr msg = { 0 };
    msg.msg_name = (void*) &sa;
    msg.msg_namelen = sizeof(sa);
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    return sendmsg(sockfd, &msg, 0);
  }

  /*_________________---------------------------__________________
    _________________     UTNLDiag_recv         __________________
    -----------------___________________________------------------
  */

  void UTNLDiag_recv(void *magic, int sockFd, UTNLDiagCB diagCB)
  {
    uint8_t recv_buf[HSP_READNL_RCV_BUF];
    int batch = 0;
    if(sockFd > 0) {
      for( ; batch < HSP_READNL_BATCH; batch++) {
	int numbytes = recv(sockFd, recv_buf, sizeof(recv_buf), 0);
	if(numbytes <= 0)
	  break;
	struct nlmsghdr *nlh = (struct nlmsghdr*) recv_buf;
	while(NLMSG_OK(nlh, numbytes)){
	  if(nlh->nlmsg_type == NLMSG_DONE)
	    break;
	  if(nlh->nlmsg_type == NLMSG_ERROR){
            struct nlmsgerr *err_msg = (struct nlmsgerr *)NLMSG_DATA(nlh);
	    // Frequently see:
	    // "device or resource busy" (especially with NLM_F_DUMP set)
	    // "netlink error" (IPv6 but connection not established)
	    // so only log when debugging:
	    myDebug(1, "Error in netlink message: %d : %s", err_msg->error, strerror(-err_msg->error));
	    break;
	  }
	  struct inet_diag_msg *diag_msg = (struct inet_diag_msg*) NLMSG_DATA(nlh);
	  int rtalen = nlh->nlmsg_len - NLMSG_LENGTH(sizeof(*diag_msg));
	  (*diagCB)(magic, sockFd, nlh->nlmsg_seq, diag_msg, rtalen);
	  nlh = NLMSG_NEXT(nlh, numbytes);
	}
      }
    }
  }
  
  /*_________________---------------------------__________________
    _________________    UTNLDiag_open          __________________
    -----------------___________________________------------------
  */

  int UTNLDiag_open(void) {
    // open the netlink monitoring socket
    int nl_sock = socket(AF_NETLINK, SOCK_DGRAM, NETLINK_INET_DIAG);
    if(nl_sock < 0) {
      myLog(LOG_ERR, "nl_sock open failed: %s", strerror(errno));
      return -1;
    }

    // set the socket to non-blocking
    int fdFlags = fcntl(nl_sock, F_GETFL);
    fdFlags |= O_NONBLOCK;
    if(fcntl(nl_sock, F_SETFL, fdFlags) < 0) {
      myLog(LOG_ERR, "NFLOG fcntl(O_NONBLOCK) failed: %s", strerror(errno));
    }

    // make sure it doesn't get inherited, e.g. when we fork a script
    fdFlags = fcntl(nl_sock, F_GETFD);
    fdFlags |= FD_CLOEXEC;
    if(fcntl(nl_sock, F_SETFD, fdFlags) < 0) {
      myLog(LOG_ERR, "NFLOG fcntl(F_SETFD=FD_CLOEXEC) failed: %s", strerror(errno));
    }

    return nl_sock;
  }


#if defined(__cplusplus)
} /* extern "C" */
#endif
