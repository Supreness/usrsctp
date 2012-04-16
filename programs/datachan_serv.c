/*
 * Copyright (C) 2012 Michael Tuexen
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the project nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE PROJECT AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.	IN NO EVENT SHALL THE PROJECT OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/*
 * Usage: daytime_server [local_encaps_port] [remote_encaps_port]
 */

#if defined(__Userspace_os_Windows)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#if !defined(__Userspace_os_Windows)
#include <unistd.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#include <netinet/sctp_pcb.h>
#include <usrsctp.h>

#include "datachan.h"

#define SIZEOF_ARRAY(x) (sizeof(x)/sizeof((x)[0]))

#define MAX_INPUT_LINE 1024
#define MAX_CHANNELS 100
#define INVALID_STREAM 0xFFFF

struct {
  int8_t   pending;
  uint8_t  channel_type;
  uint16_t flags;
  uint16_t reverse;
  uint16_t reliability_params;
  /* FIX! label */
} channels_out[MAX_CHANNELS];

struct {
  uint8_t in_use;
} channels_in[MAX_CHANNELS];

int num_channels = 0;

void
send_error_response(struct socket* sock,
                    struct rtcweb_datachannel_msg *msg,
                    struct sctp_queued_to_read *control,
                    uint16_t error)
{
  struct rtcweb_datachannel_msg response[2]; // need extra space for error value

  /* ok, send a response */
  response[0] = *msg;
  response[0].msg_type = DATA_CHANNEL_OPEN_RESPONSE;
  response[0].reverse_stream = control->sinfo_stream;
  *((uint16_t *) &((&msg->reliability_params)[1])) = htons(error);

  if (userspace_sctp_sendmsg(sock, &response[0], sizeof(response[0])+sizeof(uint16_t),
                             NULL, 0,
                             DATA_CHANNEL_PPID_CONTROL,0,
                             control->sinfo_stream, 0, 0) < 0)
  {
    printf("error %d sending response\n",errno);
    /* hard to send an error here... */
  }
}

static int
receive_cb(struct socket* sock, struct sctp_queued_to_read *control)
{
	if (control == NULL) {
		/* done = 1;*/ /* XXX? */
		userspace_close(sock);
	} else {
    struct mbuf *m;
    struct rtcweb_datachannel_msg *msg;
    uint16_t forward,reverse;
    uint16_t error;
    int i;

    switch (control->sinfo_ppid)
    {
      case DATA_CHANNEL_PPID_CONTROL:
        m = control->data;
        if (SCTP_BUF_LEN(m) < sizeof(control->length)) {
          if ((m = m_pullup(m, control->length)) == 0) {
            printf("Can not get the datachannel msg in the first mbuf.\n");
            return 0;
          }
          control->data = m; // used by m_freem()
        }
        msg = mtod(m, struct rtcweb_datachannel_msg *);

        printf("rtcweb_datachannel_msg = \n"
               "  type\t\t\t%u\n"
               "  channel_type\t\t%u\n"
               "  flags\t\t\t0x%04x\n"
               "  reverse_stream\t%u\n"
               "  reliability\t\t0x%04x\n"
               "  label\t\t\t%s\n",
               msg->msg_type, msg->channel_type, msg->flags,
               msg->reverse_stream, msg->reliability_params,
               (char *) &((&msg->reliability_params)[1]));

        switch (msg->msg_type)
        {
          case DATA_CHANNEL_OPEN:
            if (channels_in[control->sinfo_stream].in_use)
            {
              printf("error, channel %u in use\n",control->sinfo_stream);
              send_error_response(sock,msg,control,ERR_DATA_CHANNEL_ALREADY_OPEN);
              break;
            }
            reverse = control->sinfo_stream;
            if (channels_out[reverse].reverse == INVALID_STREAM &&
                !channels_out[reverse].pending)
            {
              channels_in[reverse].in_use   = 1;
              channels_out[reverse].reverse = reverse;
              forward = reverse;
            }
            else
            {
              /* some sort of glare, find a spare channel */
              for (i = 0; i < SIZEOF_ARRAY(channels_out); i++)
              {
                if (!(channels_out[i].reverse != INVALID_STREAM &&
                      !channels_out[i].pending))
                {
                  break;
                }
              }
              if (i >= SIZEOF_ARRAY(channels_out))
              {
                printf("no reverse channel available!\n");
                channels_in[reverse].in_use = 0;
                send_error_response(sock,msg,control,ERR_DATA_CHANNEL_NONE_AVAILABLE);
                break;
              }
              forward = i;
              channels_out[forward].reverse = reverse;
              channels_in[reverse].in_use = 1;
            }

            /* channels_out[reverse].pending = 0; */
            channels_out[forward].channel_type       = msg->channel_type;
            channels_out[forward].flags              = msg->flags;
            channels_out[forward].reliability_params = msg->reliability_params;

            /* Label is in msg_type_data */
            /* FIX! */

            {
              struct rtcweb_datachannel_msg response[2]; // need extra space for error value
              /* ok, send a response */
              response[0] = *msg;
              response[0].msg_type = DATA_CHANNEL_OPEN_RESPONSE;
              response[0].reverse_stream = reverse;
              *((uint16_t *) &((&msg->reliability_params)[1])) = /*htons*/(0); /* no error */

              if (userspace_sctp_sendmsg(sock, &response[0], sizeof(response[0])+sizeof(uint16_t),
                                         NULL, 0,
                                         DATA_CHANNEL_PPID_CONTROL,0,
                                         forward, 0, 0) < 0)
              {
                printf("error %d sending response\n",errno);
                channels_out[forward].reverse = INVALID_STREAM;
                channels_in[reverse].in_use = 0;
                /* hard to send an error here... */
                break;
              }
            }
            num_channels++;
            printf("successful open of in: %u, out: %u, total channels %d\n",
                   reverse, forward, num_channels);
            /* XXX Notify ondatachannel */
            break;

          case DATA_CHANNEL_OPEN_RESPONSE:
            if (!channels_out[msg->reverse_stream].pending)
            {
              printf("Error: open_response for non-pending channel %u (on %u)\n",
                     msg->reverse_stream, control->sinfo_stream);
              break;
            }
            error = ntohs(*((uint16_t *) &((&msg->reliability_params)[1])));
            if (error)
            {
              printf("Error: open_response for %u returned error %u\n",
                     msg->reverse_stream, error);
              break;
            }
            channels_out[msg->reverse_stream].pending = 0;
            channels_in[control->sinfo_stream].in_use = 1;
            channels_out[msg->reverse_stream].reverse = control->sinfo_stream;
            num_channels++;
            printf("successful open of in: %u, out: %u, total channels %d\n",
                   control->sinfo_stream, msg->reverse_stream, num_channels);
            /* XXX Notify onopened */
            break;

          default:
            printf("Error: Unknown message received: %u\n",msg->msg_type);
            break;
        }
        break;

      case DATA_CHANNEL_PPID_DOMSTRING:
        printf("Received DOMString, len %d\n",control->length);
        for (m = control->data; m; m = SCTP_BUF_NEXT(m)) {
          fwrite(m->m_data,SCTP_BUF_LEN(m),1,stdout);
        }
        /* XXX Notify onmessage */
        break;

      case DATA_CHANNEL_PPID_BINARY:
        printf("Received binary, len %d\n",control->length);
        {
          m = control->data;
          if (SCTP_BUF_LEN(m) < 8) {
            if ((m = m_pullup(m, 8)) == 0) {
              printf("Can not get the datachannel msg in the first mbuf.\n");
              return 0;
            }
            control->data = m; // used by m_freem()
          }
          char *buffer = m->m_data;
          printf("0000: %02x %02x %02x %02x %02x %02x %02x %02x \n",
                 buffer[0],buffer[1],buffer[2],buffer[3],
                 buffer[4],buffer[5],buffer[6],buffer[7]);
        }
        /* XXX Notify onmessage */
        break;

      default:
        printf("Error: Unknown ppid %u\n",control->sinfo_ppid);
        break;
    } /* switch ppid */

		m_freem(control->data);
	}
	return 1;
}


int
main(int argc, char *argv[])
{
	struct socket *sock, *conn_sock;
	struct sockaddr_in addr;
	struct sockaddr_in addr4;
	struct sockaddr_in6 addr6;
	struct sctp_udpencaps encaps;
	socklen_t addr_len;
  fd_set fds;
  int i;

  for (i = 0; i < SIZEOF_ARRAY(channels_out); i++)
  {
    channels_out[i].reverse = INVALID_STREAM;
    channels_out[i].pending = 0;
    channels_in[i].in_use = 0;
  }

  if (argc > 1) {
		sctp_init(atoi(argv[1]));
	} else {
		sctp_init(9899);
	}
	SCTP_BASE_SYSCTL(sctp_debug_on) = 0x0;
	SCTP_BASE_SYSCTL(sctp_blackhole) = 2;

	if ((sock = userspace_socket(AF_INET, SOCK_STREAM, IPPROTO_SCTP)) == NULL) {
		perror("userspace_socket");
	}
	if (argc > 2) {
		memset(&encaps, 0, sizeof(struct sctp_udpencaps));
		encaps.sue_address.ss_family = AF_INET;
		encaps.sue_port = htons(atoi(argv[2]));
		if (userspace_setsockopt(sock, IPPROTO_SCTP, SCTP_REMOTE_UDP_ENCAPS_PORT, (const void*)&encaps, (socklen_t)sizeof(struct sctp_udpencaps)) < 0) {
			perror("setsockopt");
		}
	}

  if (argc > 4)
  {
    /* Acting as the connector */
    printf("Connecting to %s %s\n",argv[3],argv[4]);
    memset((void *)&addr4, 0, sizeof(struct sockaddr_in));
    memset((void *)&addr6, 0, sizeof(struct sockaddr_in6));
#if !defined(__Userspace_os_Linux) && !defined(__Userspace_os_Windows)
    addr4.sin_len = sizeof(struct sockaddr_in);
    addr6.sin6_len = sizeof(struct sockaddr_in6);
#endif
    addr4.sin_family = AF_INET;
    addr6.sin6_family = AF_INET6;
    addr4.sin_port = htons(atoi(argv[4]));
    addr6.sin6_port = htons(atoi(argv[4]));
    if (inet_pton(AF_INET6, argv[3], &addr6.sin6_addr) == 1) {
      if (userspace_connect(sock, (struct sockaddr *)&addr6, sizeof(struct sockaddr_in6)) < 0) {
        perror("userspace_connect");
      }
    } else if (inet_pton(AF_INET, argv[3], &addr4.sin_addr) == 1) {
      if (userspace_connect(sock, (struct sockaddr *)&addr4, sizeof(struct sockaddr_in)) < 0) {
        perror("userspace_connect");
      }
    } else {
      printf("Illegal destination address.\n");
    }

  }
  else
  {
    /* Acting as the 'server' */
    memset((void *)&addr, 0, sizeof(struct sockaddr_in));
#ifdef HAVE_SIN_LEN
    addr.sin_len = sizeof(struct sockaddr_in);
#endif
    addr.sin_family = AF_INET;
    addr.sin_port = htons(13);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    printf("Waiting for connections on port %d\n",ntohs(addr.sin_port));
    if (userspace_bind(sock, (struct sockaddr *)&addr, sizeof(struct sockaddr_in)) < 0) {
      perror("userspace_bind");
    }
    if (userspace_listen(sock, 1) < 0) {
      perror("userspace_listen");
    }
  }

  while (sock) {
    if (argc > 4)
    {
      conn_sock = sock;
      sock = NULL;
      printf("connect() succeeded!  Entering connected mode\n");
    }
    else
    {
      addr_len = 0;
      if ((conn_sock = userspace_accept(sock, NULL, &addr_len)) == NULL) {
        continue;
      }
      printf("Accepting incoming connection.  Entering connected mode\n");
    }

    /* all incoming data will appear in receive_cb */
    register_recv_cb(conn_sock, receive_cb);

    /* control loop and sending */
    FD_ZERO(&fds);
    for(;;){
    /*
      int nr;

      FD_SET(fileno(stdin), &fds);
      if ((nr = select(fileno(stdin)+1, &fds, NULL, NULL, NULL)) < 0)
      {
        if (errno == EINTR)
          continue;
        else
        {
          printf("select error\n");
          exit(1);
        }
      }
      if (FD_ISSET(fileno(stdin), &fds) )
      {
    */
      {
        char inputline[MAX_INPUT_LINE];
        if (fgets(inputline, MAX_INPUT_LINE, stdin) == NULL) {
          /* exit on ^d */
          printf("exiting..\n");
          exit(0);
        }
        else {
          struct rtcweb_datachannel_msg msg[4]; /* cheat to get space for label */
          int stream, reliable;
          int len;
          uint32_t timeout;
          uint32_t flags;

          if (sscanf(inputline,"open %d %d:",&stream,&reliable) == 2)
          {
            if (stream < 0 || stream >= SIZEOF_ARRAY(channels_out))
            {
              printf("stream number %d out of range!\n",stream);
              continue;
            }
            if (reliable < 0 || reliable > DATA_CHANNEL_PARTIAL_RELIABLE_TIMED)
            {
              printf("reliability type %d invalid!\n",reliable);
              continue;
            }
            if (channels_out[stream].reverse != INVALID_STREAM ||
                channels_out[stream].pending != 0)
            {
              printf("channel %d already in use!\n",stream);
              continue;
            }

            channels_out[stream].pending = 1;
            channels_out[stream].channel_type = reliable;
            channels_out[stream].flags = 0; /* XXX */
            channels_out[stream].reverse = INVALID_STREAM;
            channels_out[stream].reliability_params = 0;

            msg[0].msg_type = DATA_CHANNEL_OPEN;
            msg[0].channel_type = reliable;
            msg[0].flags = 0; /* XXX */
            msg[0].reverse_stream = INVALID_STREAM;
            msg[0].reliability_params = 0;
            sprintf((char *) &((&msg[0].reliability_params)[1]),"chan %d",stream);
            len = sizeof(msg) + strlen((char *) &((&msg[0].reliability_params)[1]));

            timeout = channels_out[stream].reliability_params;
            switch(channels_out[stream].channel_type)
            {
              case DATA_CHANNEL_RELIABLE:
                flags = 0;
                break;
              case DATA_CHANNEL_UNRELIABLE:
              case DATA_CHANNEL_PARTIAL_RELIABLE_REXMIT:
                flags = SCTP_PR_SCTP_RTX | SCTP_UNORDERED;
                break;
              case DATA_CHANNEL_PARTIAL_RELIABLE_TIMED:
                flags = SCTP_PR_SCTP_TTL | SCTP_UNORDERED;
                break;
            }

            if (userspace_sctp_sendmsg(conn_sock,&msg[0], len,
                                       NULL, 0,
                                       DATA_CHANNEL_PPID_CONTROL,flags,
                                       stream, timeout, 0) < 0)
            {
              printf("error %d sending open\n",errno);
              channels_out[stream].pending = 0;
            }
          }
          else if (sscanf(inputline,"send %d:",&stream) == 1)
          {
            char *str = strchr(inputline,':');
            if (!str) /* should be impossible */
              exit(1);
            str++;
            if (userspace_sctp_sendmsg(conn_sock,str, strlen(str),
                                       NULL, 0,
                                       DATA_CHANNEL_PPID_DOMSTRING,0,
                                       stream, 0, 0) < 0)
            {
              printf("error %d sending string\n",errno);
            }
          }
          else if (sscanf(inputline,"close %d:",&stream) == 1)
          {
          }
          else
          {
            printf("unknown command '%s'\n",inputline);
          }
        }
      } /* if FDSET */
    }
		userspace_close(conn_sock);
	}
	userspace_close(sock);
	while (userspace_finish() != 0) {
#if defined (__Userspace_os_Windows)
		Sleep(1000);
#else
		sleep(1);
#endif
	}
	return (0);
}
