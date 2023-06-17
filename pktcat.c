/* arm-linux-musleabihf-cc -static -s -O2 -Wextra -Wall -pedantic -o pktcat utils/pktcat.c */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <poll.h>
#include <pthread.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <linux/if_ether.h>
#include <linux/if_packet.h>
#include <netinet/in.h>

static int flag_ifidx = 0;
static int flag_recv = 0;
static int flag_send = 0;

static const unsigned char hex2ch[] = "0123456789abcdef";
static int ch2hex(int ch)
{
  static const unsigned char ch2hexmap[] = {
      ['0']=1,  ['5']=6,
      ['1']=2,  ['6']=7,
      ['2']=3,  ['7']=8,
      ['3']=4,  ['8']=9,
      ['4']=5,  ['9']=10,
      ['a']=0xb, ['A']=0xb,
      ['b']=0xc, ['B']=0xc,
      ['c']=0xd, ['C']=0xd,
      ['d']=0xe, ['D']=0xe,
      ['e']=0xf, ['G']=0xf,
      ['f']=0x10, ['F']=0x10,
  };

  if (ch < 0 || (size_t)ch >= (sizeof ch2hexmap / sizeof ch2hexmap[0]))
    return -1;

  return ch2hexmap[ch] ? ch2hexmap[ch] - 1 : -1;
}

static int LOG(const char *fmt, ...) {
  static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
  va_list va;
  int rc = -1;

  va_start(va, fmt);
  if (!pthread_mutex_lock(&m)) {
    rc = vfprintf(stderr, fmt, va);
    putc('\n', stderr);
    pthread_mutex_unlock(&m);
  }
  va_end(va);
  return rc;
}

static int ifid2idx(const char ifid[IF_NAMESIZE], unsigned *if_idx)
{
  unsigned idx;

  if (flag_ifidx) {
    if (sscanf(ifid, "%u", &idx) != 1) {
        LOG("ifindex '%s' is not a number", ifid);
        return -1;
    }
  } else {
    if (!(idx = if_nametoindex(ifid))) {
        LOG("if_nametoindex(\"%s\"): %s", ifid, strerror(errno));
        return -1;
    }
  }
  *if_idx = idx;
  return 0;
}

static void *ifpoll(void *p) {
    char ifname[IF_NAMESIZE];
    static unsigned char pkt[10240];
    ssize_t i, psz;
    int fd = *(int *)p;

    while (1)
    {
        struct cmsghdr *cmsg;
        struct tpacket_auxdata *aux;
        long vlan_tpid = -1;
        long vlan_tci = -1;
        struct sockaddr_ll addr = {0};
        union {
            char buf[CMSG_SPACE(sizeof(struct tpacket_auxdata))];
            struct cmsghdr cmsg;
        } cmsg_buf;
        struct iovec iov = {
            .iov_base = pkt,
            .iov_len = sizeof pkt,
        };
        struct msghdr msg = {
           .msg_name = &addr,
           .msg_namelen = sizeof addr,
           .msg_iov = &iov,
           .msg_iovlen = 1,
           .msg_control = &cmsg_buf,
           .msg_controllen = sizeof cmsg_buf,
        };

        psz = recvmsg(fd, &msg, 0);
        if (psz < 0) {
            if (errno != EINTR) {
                LOG("recvmsg: %s", strerror(errno));
                exit(1);
            }
            continue;
        }

        /* find vlan hdr to re-insert */
        for (cmsg = CMSG_FIRSTHDR(&msg); cmsg; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
			if (cmsg->cmsg_len < CMSG_LEN(sizeof(struct tpacket_auxdata)) ||
			    cmsg->cmsg_level != SOL_PACKET ||
			    cmsg->cmsg_type != PACKET_AUXDATA) {
				continue;
			}
			aux = (struct tpacket_auxdata *)CMSG_DATA(cmsg);
			if (aux->tp_vlan_tci == 0 ||
                !(aux->tp_status & TP_STATUS_VLAN_VALID)) {
				continue;
			}
            vlan_tpid = ETH_P_8021Q;
            if (aux->tp_vlan_tpid || aux->tp_status & TP_STATUS_VLAN_TPID_VALID)
              vlan_tpid = aux->tp_vlan_tpid;
			vlan_tci = aux->tp_vlan_tci;
		}

        if (flag_ifidx) {
          printf("%d ", addr.sll_ifindex);
        } else {
          if (!if_indextoname(addr.sll_ifindex, ifname)) {
            LOG("if_indextoname(%d): ", addr.sll_ifindex, strerror(errno));
            continue;
          }
          printf("%s ", ifname);
        }

        for (i = 0; i < 12 && i < psz; ++i) {
          putchar(hex2ch[pkt[i] >> 4 & 0xF]);
          putchar(hex2ch[pkt[i] & 0xF]);
        }

        if (vlan_tci >= 0) {
          putchar(hex2ch[vlan_tpid >> 12 & 0xf]);
          putchar(hex2ch[vlan_tpid >> 8 & 0xf]);
          putchar(hex2ch[vlan_tpid >> 4 & 0xf]);
          putchar(hex2ch[vlan_tpid >> 0 & 0xf]);
          putchar(hex2ch[vlan_tci >> 12 & 0xf]);
          putchar(hex2ch[vlan_tci >> 8 & 0xf]);
          putchar(hex2ch[vlan_tci >> 4 & 0xf]);
          putchar(hex2ch[vlan_tci >> 0 & 0xf]);
        }

        for (; i < psz; ++i) {
          putchar(hex2ch[pkt[i] >> 4 & 0xF]);
          putchar(hex2ch[pkt[i] & 0xF]);
        }
        putchar('\n');
    }
}

int main(int argc, char *argv[])
{
    static unsigned char buf[32768];
    char *o, *ifname;
    unsigned char *pkt;
    size_t i, pktsz;
    int failed;
    int fd;
    struct sockaddr_ll addr;
    unsigned if_idx;
    pthread_t pt;
    const char *bindifname = 0;

    (void)argc;

    while ((o = *argv))
    {
       if (*o == '-') {
            if (!o[1]) {
               LOG("invalid flag");
               exit(1);
            }
            while (*++o) {
               if (*o == 'i')
                 flag_ifidx = !flag_ifidx;
               else if (*o == 'r')
                 flag_recv = !flag_recv;
               else if (*o == 's')
                 flag_send = !flag_send;
               else if (*o == 'I') {
                 if (o[1] || !(bindifname = *++argv)) {
                   LOG("incomplete flag '%c'", *o);
                   exit(1);
                 }
                 break;
               } else {
                   LOG("invalid flag '%c'", *o);
                   exit(1);
               }
            }
       }
       ++argv;
    }

    if ((fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ALL))) < 0) {
        LOG("socket: %s", strerror(errno));
        exit(1);
    }

    if (setsockopt(fd, SOL_PACKET, PACKET_AUXDATA, &(int){1}, sizeof(int)) < 0)
    {
        LOG("setsockopt(SOL_PACKET): %s", strerror(errno));
        exit(1);
    }

    if (bindifname) {
        if (ifid2idx(bindifname, &if_idx)) {
            exit(1);
        }
        addr = (struct sockaddr_ll){
         .sll_family = AF_PACKET,
         .sll_ifindex = if_idx,
        };
        if (bind(fd, (struct sockaddr *)&addr, sizeof addr)) {
          LOG("bind: %s", strerror(errno));
          exit(1);
        }
    }

    if (flag_recv) {
      if (pthread_create(&pt, 0, ifpoll, &fd)) {
          LOG("pthread_create: %s", strerror(errno));
          exit(1);
      }
    }

    /* input format: ifname pkthex-string\n */
    while (flag_send && fgets((char *)buf, sizeof buf, stdin))
    {
        failed = 0;

        ifname = (char *)buf;
        for (i = 0; ifname[i] && ifname[i] != ' '; ++i);
        ifname[i] = 0;

        while (buf[++i] == ' ');

        pkt = &buf[i];
        for (pktsz = 0, i = 0; pkt[i] && pkt[i + 1]; ++pktsz, i += 2) {
            int msb = ch2hex(pkt[i]);
            int lsb = ch2hex(pkt[i + 1]);

            if (msb < 0 || lsb < 0) {
                failed = 1;
                break;
            }
            pkt[pktsz] = msb << 4 | lsb;
        }
        if (failed)
            continue;

        if (ifid2idx(ifname, &if_idx))
            continue;

        addr = (struct sockaddr_ll){
          .sll_ifindex = if_idx,
        };

        while (1) {
          errno = 0;
          if (sendto(fd, pkt, pktsz, 0,
                (struct sockaddr*)&addr, sizeof addr) < 0) {
            if (errno == EINTR)
              continue;
            LOG("sendto: %s", strerror(errno));
          }
          break;
        }
    }

    if (flag_recv)
      pthread_join(pt, 0);

    return 0;
}
