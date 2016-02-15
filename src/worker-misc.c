/*
 * Copyright (C) 2013, 2014 Nikos Mavrogiannopoulos
 *
 * This file is part of ocserv.
 *
 * ocserv is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * ocserv is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include <gnutls/gnutls.h>
#include <gnutls/crypto.h>
#include <gnutls/x509.h>
#include <errno.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>

#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>

#include <vpn.h>
#include <worker.h>
#include <tlslib.h>

#ifdef HAVE_SIGALTSTACK
# include <signal.h>
# include <sys/mman.h>
#endif

/* recv from the new file descriptor and make sure we have a valid packet */
static int recv_from_new_fd(struct worker_st *ws, int fd, UdpFdMsg *tmsg)
{
	int saved_fd, ret;
	UdpFdMsg *saved_tmsg;

	/* don't bother with anything if we are on uninitialized state */
	if (ws->dtls_session == NULL || ws->udp_state != UP_ACTIVE)
		return 1;

	saved_fd = ws->dtls_tptr.fd;
	saved_tmsg = ws->dtls_tptr.msg;

	ws->dtls_tptr.msg = tmsg;
	ws->dtls_tptr.fd = fd;

	ret = gnutls_record_recv(ws->dtls_session, ws->buffer, ws->buffer_size);
	/* we receive GNUTLS_E_AGAIN in case the packet was discarded */
	if (ret > 0) {
		ret = 1;
		goto revert;
	}

	ret = 0;
 revert:
 	ws->dtls_tptr.fd = saved_fd;
 	ws->dtls_tptr.msg = saved_tmsg;
 	return ret;
}

int handle_commands_from_main(struct worker_st *ws)
{
	struct iovec iov[3];
	uint8_t cmd;
	uint16_t length;
	int e;
	struct msghdr hdr;
	uint8_t cmd_data[1536];
	UdpFdMsg *tmsg = NULL;
	union {
		struct cmsghdr    cm;
		char              control[CMSG_SPACE(sizeof(int))];
	} control_un;
	struct cmsghdr  *cmptr;
	int ret;
	/*int cmd_data_len;*/

	memset(&cmd_data, 0, sizeof(cmd_data));

	iov[0].iov_base = &cmd;
	iov[0].iov_len = 1;

	iov[1].iov_base = &length;
	iov[1].iov_len = 2;

	iov[2].iov_base = cmd_data;
	iov[2].iov_len = sizeof(cmd_data);

	memset(&hdr, 0, sizeof(hdr));
	hdr.msg_iov = iov;
	hdr.msg_iovlen = 3;

	hdr.msg_control = control_un.control;
	hdr.msg_controllen = sizeof(control_un.control);

	do {
		ret = recvmsg( ws->cmd_fd, &hdr, 0);
	} while(ret == -1 && errno == EINTR);
	if (ret == -1) {
		e = errno;
		oclog(ws, LOG_ERR, "cannot obtain data from command socket: %s", strerror(e));
		exit_worker(ws);
	}

	if (ret == 0) {
		oclog(ws, LOG_ERR, "parent terminated");
		return ERR_NO_CMD_FD;
	}

	if (length > ret - 3) {
		oclog(ws, LOG_DEBUG, "worker received invalid message %s of %u bytes that claims to be %u\n", cmd_request_to_str(cmd), (unsigned)ret-3, (unsigned)length);
		exit_worker(ws);
	} else {
		oclog(ws, LOG_DEBUG, "worker received message %s of %u bytes\n", cmd_request_to_str(cmd), (unsigned)length);
	}

	/*cmd_data_len = ret - 1;*/

	switch(cmd) {
		case CMD_TERMINATE:
			exit_worker(ws);
		case CMD_UDP_FD: {
			unsigned has_hello = 1;
			int fd;

			if (ws->udp_state != UP_WAIT_FD) {
				oclog(ws, LOG_DEBUG, "received another a UDP fd!");
			}

			tmsg = udp_fd_msg__unpack(NULL, length, cmd_data);
			if (tmsg) {
				has_hello = tmsg->hello;
			}

			if ( (cmptr = CMSG_FIRSTHDR(&hdr)) != NULL && cmptr->cmsg_len == CMSG_LEN(sizeof(int))) {
				if (cmptr->cmsg_level != SOL_SOCKET || cmptr->cmsg_type != SCM_RIGHTS || CMSG_DATA(cmptr) == NULL) {
					oclog(ws, LOG_ERR, "received UDP fd message of wrong type");
					goto udp_fd_fail;
				}

				memcpy(&fd, CMSG_DATA(cmptr), sizeof(int));
				set_non_block(fd);

				if (has_hello == 0) {
					/* check if the first packet received is a valid one -
					 * if not discard the new fd */
					if (!recv_from_new_fd(ws, fd, tmsg)) {
						oclog(ws, LOG_INFO, "received UDP fd message but its session has invalid data!");
						if (tmsg)
							udp_fd_msg__free_unpacked(tmsg, NULL);
						close(fd);
						return 0;
					}
				} else { /* received client hello */
					ws->udp_state = UP_SETUP;
				}

				if (ws->dtls_tptr.fd != -1)
					close(ws->dtls_tptr.fd);
				if (tmsg && ws->dtls_tptr.msg != NULL)
					udp_fd_msg__free_unpacked(ws->dtls_tptr.msg, NULL);

				ws->dtls_tptr.msg = tmsg;
				ws->dtls_tptr.fd = fd;

				oclog(ws, LOG_DEBUG, "received new UDP fd and connected to peer");
				return 0;
			} else {
				oclog(ws, LOG_ERR, "could not receive peer's UDP fd");
				return -1;
			}

			}
			break;
		default:
			oclog(ws, LOG_ERR, "unknown CMD 0x%x", (unsigned)cmd);
			exit_worker(ws);
	}

	return 0;

udp_fd_fail:
	if (tmsg)
		udp_fd_msg__free_unpacked(tmsg, NULL);
	if (ws->dtls_tptr.fd == -1)
		ws->udp_state = UP_DISABLED;

	return -1;
}

/* Completes the VPN device information.
 * 
 * Returns 0 on success.
 */
int complete_vpn_info(worker_st * ws, struct vpn_st *vinfo)
{
	int ret, fd;
	struct ifreq ifr;

	if (vinfo->ipv4 == NULL && vinfo->ipv6 == NULL) {
		return -1;
	}

	if (ws->config->network.mtu != 0) {
		vinfo->mtu = ws->config->network.mtu;
	} else {
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd == -1)
			return -1;

		memset(&ifr, 0, sizeof(ifr));
		ifr.ifr_addr.sa_family = AF_INET;
		snprintf(ifr.ifr_name, IFNAMSIZ, "%s", vinfo->name);
		ret = ioctl(fd, SIOCGIFMTU, (caddr_t) & ifr);
		if (ret < 0) {
			oclog(ws, LOG_INFO,
			      "cannot obtain MTU for %s. Assuming 1500",
			      vinfo->name);
			vinfo->mtu = 1500;
		} else {
			vinfo->mtu = ifr.ifr_mtu;
		}
		close(fd);
	}

	return 0;
}

void ocsigaltstack(struct worker_st *ws)
{
#if defined(HAVE_SIGALTSTACK) && defined(HAVE_POSIX_MEMALIGN)
	stack_t ss;
	int e;

	/* setup the stack for signal handlers */
	if (posix_memalign(&ss.ss_sp, getpagesize(), SIGSTKSZ) < 0) {
		oclog(ws, LOG_ERR,
		      "could not allocate memory for signal stack");
		exit(1);
	}
	if (mprotect(ss.ss_sp, SIGSTKSZ, PROT_READ|PROT_WRITE) == -1) {
		e = errno;
		oclog(ws, LOG_ERR, "mprotect: %s\n", strerror(e));
		exit(1);
	}
	ss.ss_size = SIGSTKSZ;
	ss.ss_flags = 0;
	if (sigaltstack(&ss, NULL) == -1) {
		e = errno;
		oclog(ws, LOG_ERR, "sigaltstack: %s\n", strerror(e));
		exit(1);
	}
#endif
}
