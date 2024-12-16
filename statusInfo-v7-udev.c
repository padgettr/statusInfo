/* Status bar text
 * Version 7.3
 *
 * Show standard status info:
 * Periodic update:
 *    network status (wlan signal, eth connection speed); temperature readout; battery charge %; battery discharge power usage; date and time.
 * Event driven notifications:
 *    Defined alsa mixer controls in config.h
 *    Defined udev subsystems  in config.h
 *
 * This version uses libnl to obtain the wifi signal level. Documentation can be found here:
 * https://github.com/thom311/libnl/releases/download/libnl3_9_0/libnl-doc-3.9.0.tar.gz
 * Network status code based on the example in linux man 3 getifaddrs and netlink code based on
 * iw tool: http://git.sipsolutions.net/iw.git/
 *
 * Compile with:
 *    gcc -Wall -I/usr/include/libnl3 statusInfo-v7-udev.c -lnl-genl-3 -lnl-3 -lX11 -ludev -lasound -o statusInfo
 *
 * Dr. R. Padgett <rod_padgett@hotmail.com> (c) 2024
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/signalfd.h>
#include <poll.h>
#include <errno.h>
#include <X11/Xlib.h>
#include <pwd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <libudev.h>
#include <alsa/asoundlib.h>

/* ethtool */
#include <ifaddrs.h>
#include <net/if.h>     /* For IFF flags */
#include <sys/ioctl.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>

/* netlink */
#include <netlink/netlink.h>
#include <netlink/genl/genl.h>  /* genl_connect, genlmsg_put */
#include <netlink/genl/family.h>
#include <netlink/genl/ctrl.h>  /* genl_ctrl_resolve */
#include <linux/nl80211.h>

#define MX_PATH_LEN 256 /* Maximum length of any path including null termination. */
#define LENGTH(X) (sizeof X / sizeof X[0])   /* From dwm.c: https://suckless.org/ */

typedef struct {
   char *subSystem;
   int (*func)(struct udev_device *dev, char *displayInfo);   /* Callback should return a string for display */
} si_udevActions;

#include "config.h"

typedef struct {
   int id;
   struct nl_sock *socket;
   struct nl_cb *wlanStats_cb;
   int wlanStatsResult;
} si_nlData;

typedef struct {
   unsigned int ifindex;
   int signal;
} si_wStats;

enum { none, xorg, text, dwlb };
static Display *dpy;
static char volumeLevel[MX_STATUS_CHARS];

static int finish_handler(struct nl_msg *msg, void *arg) {
   int *ret = arg;
   *ret = 0;
   return NL_SKIP;
}

static int ack_handler(struct nl_msg *msg, void *arg) {
   fprintf(stderr, "ack_handler\n");
   return NL_STOP;
}

/* Based on station.c from iw */
static int getWifiStats_nl_cb(struct nl_msg *msg, void *arg) {
   struct nlattr *tb[NL80211_ATTR_MAX + 1];
   struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
   struct nlattr *sinfo[NL80211_STA_INFO_MAX + 1];
   struct nla_policy stats_policy[NL80211_STA_INFO_MAX + 1] = {
             [NL80211_STA_INFO_SIGNAL]      = { .type = NLA_U8 },
          };

   nla_parse(tb, NL80211_ATTR_MAX, genlmsg_attrdata(gnlh, 0),
             genlmsg_attrlen(gnlh, 0), NULL);

   /* This comment is retained from iw; station.c
    * TODO: validate the interface and mac address!
    * Otherwise, there's a race condition as soon as
    * the kernel starts sending station notifications.
    */

   if (!tb[NL80211_ATTR_STA_INFO]) {
     fprintf(stderr, "sta stats missing!\n");
     return NL_SKIP;
   }

   if (nla_parse_nested(sinfo, NL80211_STA_INFO_MAX, tb[NL80211_ATTR_STA_INFO], stats_policy)) {
     fprintf(stderr, "failed to parse nested attributes!\n");
     return NL_SKIP;
   }

   if (sinfo[NL80211_STA_INFO_SIGNAL]) {
     ((si_wStats*)arg)->signal = (int8_t)nla_get_u8(sinfo[NL80211_STA_INFO_SIGNAL]);
   }
   else
    ((si_wStats*)arg)->signal = 0;

   return NL_SKIP;
}

/* If using ss -f netlink to view sockets created by this program, port number will not be process ID. From libnl docs:
 * "...it was common practice to use the process identifier (PID) as the local port number. This became unpractical
 * with the introduction of threaded netlink applications and applications requiring multiple sockets. Therefore libnl
 * generates unique port numbers based on the process identifier and adds an offset to it allowing for multiple
 * sockets to be used."
 */
static int init_nl80211(si_nlData *nlData, si_wStats *w) {
   nlData->id=-1;
   nlData->socket = nl_socket_alloc();
   if (!nlData->socket) {
      fprintf(stderr, "Failed to allocate netlink socket.\n");
      return -1;
   }
   nl_socket_set_buffer_size(nlData->socket, 8192, 8192);

   if (genl_connect(nlData->socket)) {
      fprintf(stderr, "Failed to connect to netlink socket.\n");
      nl_close(nlData->socket);
      nl_socket_free(nlData->socket);
      return -1;
   }

   nlData->id = genl_ctrl_resolve(nlData->socket, "nl80211");
   if (nlData->id<0) {
      fprintf(stderr, "Nl80211 interface not found.\n");
      nl_close(nlData->socket);
      nl_socket_free(nlData->socket);
      return -1;
   }

   nlData->wlanStats_cb = nl_cb_alloc(NL_CB_DEFAULT);
   if ( ! nlData->wlanStats_cb) {
      fprintf(stderr, "Failed to allocate netlink callback.\n");
      nl_close(nlData->socket);
      nl_socket_free(nlData->socket);
      return -1;
   }

   nl_cb_set(nlData->wlanStats_cb, NL_CB_VALID , NL_CB_CUSTOM, getWifiStats_nl_cb, w);
   nl_cb_set(nlData->wlanStats_cb, NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, &(nlData->wlanStatsResult));
   nl_cb_set(nlData->wlanStats_cb, NL_CB_ACK, NL_CB_CUSTOM, ack_handler, NULL);

   return nlData->id;
}

static int getWifiStatus(si_nlData *nlData, si_wStats *wStats) {
   if (wStats->ifindex < 0)
      return -1;

   struct nl_msg* msg = nlmsg_alloc();
   if (!msg) {
      fprintf(stderr, "Failed to allocate netlink message.\n");
      return -2;
   }

   nlData->wlanStatsResult=1;
   genlmsg_put(msg, NL_AUTO_PORT, NL_AUTO_SEQ, nlData->id, 0, NLM_F_DUMP, NL80211_CMD_GET_STATION, 0);
   nla_put_u32(msg, NL80211_ATTR_IFINDEX, wStats->ifindex);
   nl_send_auto(nlData->socket, msg);
   while (nlData->wlanStatsResult > 0) /* Wait for NL_CB_FINISH */
      nl_recvmsgs(nlData->socket, nlData->wlanStats_cb);
   nlmsg_free(msg);

  return 0;
}

/* Ethtool: notes in /usr/include/linux/ethtool.h
 * Also see ethtool.c do_ioctl_glinksettings().
 * ifr struct in man 7 netdevice
 * Note: ETHTOOL_GSET is depreciated. Use ETHTOOL_GLINKSETTINGS.
 */
static void getEthernetStatus(char *name, char *displayStatus) {
   int fd;
   struct ifreq ifr;
   struct {
      struct ethtool_link_settings req;
      __u32 link_mode_data[3*SCHAR_MAX];   /* Store for 3x link mode bitmaps (supported, advertising, lp_advertising) - not used here. In ethtool.c size is (3 x ETHTOOL_LINK_MODE_MASK_MAX_KERNEL_NU32); ethtool internal.h defines ETHTOOL_LINK_MODE_MASK_MAX_KERNEL_NU32 as SCHAR_MAX on linux. SCHAR_MAX is defined in limits.h */
   } ecmd;
   unsigned int ifindex;

   fd=socket(AF_INET, SOCK_DGRAM, 0);
   if (fd==-1) {
      displayStatus[0]='\0';
      perror("getEthernetStatus()");
      return;
   }
   ifindex=if_nametoindex(name);

   memset(&ecmd, 0, sizeof(ecmd));        /* Set all fields of ecmd.req to zero */
   ecmd.req.cmd = ETHTOOL_GLINKSETTINGS;  /* Set required command */
   memset(&ifr, 0, sizeof(ifr));
   strncpy(ifr.ifr_name, name, IFNAMSIZ); /* Set name of interface we are interested in */
   ifr.ifr_name[IFNAMSIZ-1]='\0';
   ifr.ifr_data = (void*)&ecmd;
   if (ioctl(fd, SIOCETHTOOL, &ifr) != -1) {       /* Send all fields zero except cmd to request link mode data size from kernel */
      if (ecmd.req.link_mode_masks_nwords < 0) {   /* Field returned negative to indicate requested size (i.e. 0) unsupported; absolute value is the supported size */
         ecmd.req.cmd = ETHTOOL_GLINKSETTINGS;     /* Now get the real data using returned link_mode_masks_nwords size */
         ecmd.req.link_mode_masks_nwords = -ecmd.req.link_mode_masks_nwords;
         if (ioctl(fd, SIOCETHTOOL, &ifr) != -1) {
            snprintf(displayStatus, 16, "%c%i:%iM ", name[0], ifindex, ecmd.req.speed);
         }
      }
   }
   else {
      snprintf(displayStatus, 16, "%c(%i):err", name[0], ifindex);
      fprintf(stderr, "%s (index %i):", ifr.ifr_name, ifindex);
      perror("getEthernetStatus()");
   }
   close(fd);
}

/* ifa_flags are defined in: man 7 netdevice
 * sa_family is defined in: man 2 socket
 * examples in man 3 getifaddrs
 */
static void getNetwork(char *displayText, si_nlData *nlData, si_wStats *wStats) {
   struct ifaddrs *ifaddr;
   char displayStatus[16];
   int i=MX_ELEMENT_CHARS-1;   /* Maximum length of displayText excluding terminating NULL char */
   int j=0, n=0;
   char *names[16];
   
   if (getifaddrs(&ifaddr) == -1) {
      perror("getifaddrs");
      return;
   }

   memset(names, 0, 16*sizeof(char*));
  /* Walk through linked list, maintaining head pointer so we can free list later.*/
   for (struct ifaddrs *ifa = ifaddr; ifa != NULL && i>0; ifa = ifa->ifa_next) {
      if (ifa->ifa_addr == NULL || ifa->ifa_flags&IFF_LOOPBACK || ifa->ifa_flags&IFF_POINTOPOINT)
         continue;

      if (ifa->ifa_addr->sa_family==AF_INET || ifa->ifa_addr->sa_family==AF_INET6) {
         if (ifa->ifa_flags&IFF_RUNNING) {
            /* Match interfaces; getifaddrs() returns struct per address - we only
             * want to report the interface once
             */
            for (j=0; j<n && names[j]!=NULL; j++) {
               if (strcmp(ifa->ifa_name, names[j])==0)
                  break;
            }
            if (j<n) /* Already have this interface */
               continue;
            names[n]=ifa->ifa_name;
            if (n<15) n++;
            //printf("%i: Name: %s\n",n,names[j]);
            //printf("%s: Up\n", ifa->ifa_name);
            /* Select interfaces for display: assume 'e' for ethernet, 'b' for bridge interfaces, 'w' for wireless */
            if (ifa->ifa_name[0]=='e' || ifa->ifa_name[0]=='b')
               getEthernetStatus(ifa->ifa_name, displayStatus);
            if (ifa->ifa_name[0]=='w' && nlData->id>=0) {
               wStats->ifindex=if_nametoindex(ifa->ifa_name);
               getWifiStatus(nlData, wStats);
               snprintf(displayStatus, 16, "w%i:%ddBm ", wStats->ifindex, wStats->signal);
            }
            strncat(displayText, displayStatus, i);   /* Always adds '\0' */
            i-=strlen(displayStatus);
            if (i<0)
               break; /* Too much data */
         }
         //else
            //printf("%s: Down\n", ifa->ifa_name);
      }
   }
   freeifaddrs(ifaddr);
}

static void setXorgBarText(char *str) {
   XStoreName(dpy, DefaultRootWindow(dpy), str);
   XSync(dpy, False);
}

/* The code below is taken from dwlb, called when the -status command is given in dwlb.
 */

/* Returns -1 on error or number of bytes sent */
static int dwlbSend(struct sockaddr_un *sock_address, const char *output, const char *cmd, const char *data) {
   size_t len;
   char sockbuf[4096];
   int r;
   int sock_fd;

   if (data!=NULL)
      snprintf(sockbuf, sizeof(sockbuf), "%s %s %s", output, cmd, data);
   else
      snprintf(sockbuf, sizeof(sockbuf), "%s %s", output, cmd);

   len=strlen(sockbuf);

   sock_fd=socket(sock_address->sun_family, SOCK_STREAM, 1);
   if (sock_fd==-1) {
      perror("dwlbSend: Opening socket");
      return -1;
   }

   r=connect(sock_fd, (struct sockaddr *) sock_address, sizeof(*sock_address));
   if (r==-1) {
      perror("dwlbSend:connect");
      return -1;
   }

   r=send(sock_fd, sockbuf, len, 0);
   if (r==-1)
      perror("dwlbSend:send");

   close(sock_fd);
   return r;
}

static void getTime(char *buf, char *fmt) {
   time_t ctime;
   struct tm *ltime;

   ctime=time(NULL);
   ltime=localtime(&ctime);
   if (ltime!=NULL) {
      if (strftime(buf, MX_ELEMENT_CHARS, fmt, ltime)==0)
         snprintf(buf, MX_ELEMENT_CHARS, "[clock format error]");
   }
   else
      snprintf(buf, MX_ELEMENT_CHARS, "[clock error]");

   buf[MX_ELEMENT_CHARS-1]='\0';
}

long getSysInfo(char *sysPath) {
   FILE *fd;
   long sysInfo=-1;

   fd = fopen(sysPath, "r");
   if (fd != NULL) {
      fscanf(fd, "%li", &sysInfo);
      fclose(fd);
   }

   return sysInfo;
}

long getTmpInfo(char *sysfsThermalPath) {
   long tmp;

   tmp=getSysInfo(sysfsThermalPath);
   if (tmp!=-1)
      return tmp/1000;
   else
      return -1;
}

void getStatusInfo(char *sBuf, char *sysfsBatteryCapacity, char *sysfsBatteryPowerNow, char *sysfsThermalPath, si_nlData *nlData, si_wStats *wStats) {
   long batCapacityNow;
   long powerNow;
   char net[MX_ELEMENT_CHARS];
   char tmp[MX_ELEMENT_CHARS];
   char pwr[MX_ELEMENT_CHARS];
   char bat[MX_ELEMENT_CHARS];
   char tml[MX_ELEMENT_CHARS];

   /* Show standard status info */
   getTime(tml, "%d-%m-%Y %R");
   batCapacityNow=getSysInfo(sysfsBatteryCapacity);
   powerNow=getSysInfo(sysfsBatteryPowerNow)/1000000;
   net[0]='\0';
   tmp[0]='\0';
   pwr[0]='\0';
   bat[0]='\0';

   if (nlData->id>=0)
      getNetwork(net, nlData, wStats);

   if (sysfsThermalPath != NULL)
      snprintf(tmp, MX_ELEMENT_CHARS, "tmp:%liC%c", getTmpInfo(sysfsThermalPath), si_separator);

   if (powerNow>0)
      snprintf(pwr, MX_ELEMENT_CHARS, "pwr:%liW%c", powerNow, si_separator);

   if (batCapacityNow>15)
      snprintf(bat, MX_ELEMENT_CHARS, "bat:%li%%%c", batCapacityNow, si_separator);
   else {
      if (batCapacityNow!=-1)
         snprintf(bat, MX_ELEMENT_CHARS, "[!]bat:%li%%%c", batCapacityNow, si_separator);
   }

   snprintf(sBuf, MX_STATUS_CHARS, "%s%s%s%s%s", net, tmp, pwr, bat, tml);
   sBuf[MX_STATUS_CHARS-1]='\0';

}

/* Get thermal path for cpu */
int getThermalPath() {
   int i;
   char thermalType[16];   /* Store for thermal zone name */
   char sysfsThermalPath[MX_PATH_LEN];  /* THERMAL_ZONE<n>/name */
   FILE *thermalZone;

   for (i=0; i<9; i++) {
      if (snprintf(sysfsThermalPath, MX_PATH_LEN, "%s%i/name", THERMAL_ZONE, i) < MX_PATH_LEN)
         fprintf(stderr, "Checking thermal zone type: %s ... ", sysfsThermalPath);
      else {
         fprintf(stderr, "ERROR: getThermalPath: MX_PATH_LEN exceeded.");
         break;
      }
      thermalZone=fopen(sysfsThermalPath, "r");
      if (thermalZone==NULL) {
         fprintf(stderr, "zone not found: temperature readout not available\n");
         break;   /* No more zones */
      }
      thermalType[0]='\0';
      fgets(thermalType, 15, thermalZone);
      fclose(thermalZone);
      fprintf(stderr, "%s\n", thermalType);
      if (strstr(THERMAL_NAME, thermalType) != NULL) {
         fprintf(stderr, "Matched: using %s\n", thermalType);
         return i;
      }
   }
   return -1;
}

int dwlbSocketInit(long dwlb_ref, struct sockaddr_un *sock_address) {
   char *xdgRunTimeDir;
   int i=0, j=-1;

   /* Establish socket and check connection to dwlb */
   if (!(xdgRunTimeDir=getenv("XDG_RUNTIME_DIR"))) {
      fprintf(stderr, "dwlbSocketInit: Could not retrieve XDG_RUNTIME_DIR\n");
      return -1;
   }
   sock_address->sun_family=AF_UNIX;
   snprintf(sock_address->sun_path, sizeof (sock_address->sun_path), "%s/dwlb/dwlb-%li", xdgRunTimeDir, dwlb_ref);

   fprintf(stderr, "Waiting for dwlb socket on %s\n", sock_address->sun_path);
   for(i=0; j==-1 && i<10; i++) {
      sleep(1);
      j=dwlbSend(sock_address, "all", "status", "dwl");
   }
   if (j==-1) {
      fprintf(stderr, "dwlbSocketInit: Could not comminicate with dwlb on %s\n", sock_address->sun_path);
      return -1;
   }
   xdgRunTimeDir=NULL;

   return 0;
}

static int sbOut(int outputFunction, struct sockaddr_un *sock_address, char *status) {
   int retVal=0;

   switch (outputFunction) {
      case xorg: setXorgBarText(status); break;
      case text: printf("%s\n", status); break;
      case dwlb: retVal=dwlbSend(sock_address, "all", "status", status); break;
      default:
         fprintf(stderr, "statusInfo: sbOut: ERROR: unknown outputFunction\n");
         retVal=-1;
      break;
   }
   fflush(stdout);   /* This is required for tmux */
   return retVal;
}

/* Only supports stereo (front left / right) channels */
int mixer_elem_cb(snd_mixer_elem_t *elem, unsigned int mask) {
   long min=0, max=1, volR=-1, volL=-1;
   int rR, rL, r;
   int activeL=-1, activeR=-1, masterL=-1, masterR=-1;

   if ( ! mask & SND_CTL_EVENT_MASK_VALUE) {
      printf("Not SND_CTL_EVENT_MASK_VALUE!\n");
      return 0;
   }

   if (snd_mixer_selem_has_playback_switch(elem)) {
      rL=snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_FRONT_LEFT, &activeL);
      if (rL < 0 )
         fprintf( stderr, "Left: snd_mixer_selem_get_playback_switch: %s\n", snd_strerror(rL));
      if (snd_mixer_selem_has_playback_switch_joined(elem)==1)
         activeR=activeL;
      else {
         rR=snd_mixer_selem_get_playback_switch(elem, SND_MIXER_SCHN_FRONT_RIGHT, &activeR);
         if (rR < 0 )
            fprintf(stderr, "Right: snd_mixer_selem_get_playback_switch: %s\n", snd_strerror(rR));
      }
   }
   if (snd_mixer_selem_has_playback_volume(elem)) {
      r = snd_mixer_selem_get_playback_volume_range(elem, &min, &max);
      if (r < 0)
         fprintf(stderr, "snd_mixer_selem_get_playback_volume_range: %s\n", snd_strerror(r));

      rL=snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_LEFT, &volL);
      if (rL < 0)
         fprintf(stderr, "Left: snd_mixer_selem_get_playback_volume: %s\n", snd_strerror(rL));
      if (snd_mixer_selem_has_playback_volume_joined(elem)==1)
         volR=volL;
      else {
         rR=snd_mixer_selem_get_playback_volume(elem, SND_MIXER_SCHN_FRONT_RIGHT, &volR);
         if (rR < 0)
            fprintf(stderr, "Right: snd_mixer_selem_get_playback_volume: %s\n", snd_strerror(rR));
      }
   }
   volR-=min;
   volL-=min;
   max-=min;
   if (max > 0) {
      masterR = 100 * volR / max;
      masterL = 100 * volL / max;
   }

   r=snprintf(volumeLevel, MX_STATUS_CHARS, "%s: %s%d%%", snd_mixer_selem_get_name(elem), (activeL==1) ? "": "!", masterL);
   if (masterL!=masterR || activeL!=activeR)
      snprintf(volumeLevel+r, MX_STATUS_CHARS-r-1, ":%s%d%%", (activeR==1) ? "": "!", masterR);

   return 0;
}

static struct udev_monitor *udevInit(struct udev *udevCtx) {
   struct udev_monitor *udevMon;
   int i=0, j=0;

   udevMon=udev_monitor_new_from_netlink(udevCtx, "udev");
   if (udevMon==NULL)
      return NULL;

   for (i=0; i<LENGTH(udevActions); i++) {
      if (udev_monitor_filter_add_match_subsystem_devtype(udevMon, udevActions[i].subSystem, NULL)<0)
         fprintf(stderr, "udevInit(): Failed to add filter for %s\n", udevActions[i].subSystem);
      else
         j++;
   }

   if (j==0) {
      fprintf(stderr, "udevInit(): Failed to add any filters: aborting.\n");
      return udev_monitor_unref(udevMon);  /* Always returns NULL */
   }
   
   if (udev_monitor_enable_receiving(udevMon) < 0) {
      fprintf(stderr, "udevInit(): Unable to monitor for udev events: aborting.\n");
      return udev_monitor_unref(udevMon);
   }
   
   return udevMon;
}

/* NOTE: It says here: http://cholla.mmto.org/computers/usb/OLD/tutorial_usbloger.html
 *       that "All the strings which come from sysfs are Unicode UTF-8. It is an error to assume that they are ASCII."
 */
static int udevStatus(char *sBuf, char udevDisplayInfo[MX_NUMBER_ELEMENTS][MX_ELEMENT_CHARS], struct udev_device *dev) {
   const char *udevSubsystem=udev_device_get_subsystem(dev);
   const char *udevAction=udev_device_get_action(dev);
   int i, j, ret=-1;
   char separator=' ';
//   const char *udevPath=udev_device_get_syspath(dev);

   if (udevSubsystem==NULL)
      return -1;

   if (strcoll("change", udevAction)==0) {
      for (i=0; i<LENGTH(udevActions) && i<MX_NUMBER_ELEMENTS; i++) {
         if (strcoll(udevActions[i].subSystem, udevSubsystem)==0)
            ret=udevActions[i].func(dev, udevDisplayInfo[i]);
      }
   }

   if (ret==-1)
      snprintf(udevDisplayInfo[MX_NUMBER_ELEMENTS-1], MX_ELEMENT_CHARS, "%s: %s: %s%c", udevSubsystem, udev_device_get_sysname(dev), udevAction, separator);

   j=MX_STATUS_CHARS;
   for (i=0; i<MX_NUMBER_ELEMENTS && j>MX_ELEMENT_CHARS; i++) {
      strncat(sBuf, udevDisplayInfo[i], MX_ELEMENT_CHARS);
      j-=strlen(udevDisplayInfo[i]);
   }
   return 0;
}

int main(int argc, char **argv) {
   int exit_request=0;
   int ret, nfd=0;
   char sBuf[MX_STATUS_CHARS];
   char sysfsThermalPath[MX_PATH_LEN];
   char sysfsBatteryCapacity[MX_PATH_LEN];
   char sysfsBatteryPowerNow[MX_PATH_LEN];
   int i;
   long dwlbSocketId=-1;
   struct sockaddr_un sock_address;
   int outputFunction=none;
   si_nlData nlData;
   si_wStats wStats;
   char udevDisplayInfo[MX_NUMBER_ELEMENTS][MX_ELEMENT_CHARS];
   struct udev *udevCtx=NULL;
   struct udev_monitor *udevMon=NULL;
   struct pollfd fds[3];
   int udev_fd=-1, signal_fd=-1;
   sigset_t sigset;
   struct signalfd_siginfo siginfo;
   struct udev_device *dev;

   snd_mixer_elem_t *elem=NULL;
   snd_mixer_t *mixerp=NULL;
   snd_mixer_selem_id_t *id=NULL;

   int timeout=1000;

   if (MX_NUMBER_ELEMENTS<1) {
      fprintf(stderr, "statusInfo: ERROR: MX_NUMBER_ELEMENTS < 1. Adjust config.h and recompile.\n");
      return 1;
   }
   if (MX_NUMBER_ELEMENTS < LENGTH(udevActions))
      fprintf(stderr, "statusInfo: WARNING: MX_NUMBER_ELEMENTS smaller than number of defined udevActions.\n Some udev events may not be reported.\n");

   if (argc>=2) {
      if (argv[1][0]=='-') {
         if (argv[1][1]=='t') {
            fprintf(stderr, "statusInfo: INFO: output to text\n");
            outputFunction=text;
         }
         else {
            fprintf(stderr, "Usage: %s <-t || socket number of dwlb>\n", argv[0]);
            fprintf(stderr, "If dwlb socket number is given, status info is written to the specified dwlb socket\n");
            fprintf(stderr, "If dwlb socket number is not given, status info is written xorg root window name (for dwm)\n");
            fprintf(stderr, "If X display not found or -t is given, status info is written out as text (for sway or tmux)\n");
            return 1;
         }
      }
      else { /* Try dwlb socket */
         dwlbSocketId=atol(argv[1]);
         if (dwlbSocketId<0) {
            fprintf(stderr, "statusInfo: ERROR: Invalid socket ID for dwlb socket.\n");
            return 1;
         }
         if (dwlbSocketInit(dwlbSocketId, &sock_address)==-1)
            return 1;
         outputFunction=dwlb;
         fprintf(stderr, "statusInfo: INFO: output to dwlb\n");
      }
   }
   else { /* Try xorg */
      dpy=XOpenDisplay(NULL);
      if (dpy!=NULL) {
         fprintf(stderr, "statusInfo: INFO: output to xorg\n");
         outputFunction=xorg;
      }
      else { /* Default to text output */
         fprintf(stderr, "statusInfo: INFO: default output to text\n");
         outputFunction=text;
      }
   }

   /* Setup udev event monitoring */
   udevCtx=udev_new();
   if (udevCtx!=NULL)
      udevMon=udevInit(udevCtx);
   if (udevMon!=NULL)
      udev_fd=udev_monitor_get_fd(udevMon);
   if (udev_fd < 0)
      fprintf(stderr, "statusInfo: WARNING: error initializing udev: udev events won't be reported.\n");

   fds[0].fd=udev_fd;   /* This will be -1 on error and poll will ignore this fd */
   fds[0].events=POLLIN|POLLERR|POLLNVAL;
   nfd++;

   /* Setup netlink for wifi stats */
   if (init_nl80211(&nlData, &wStats) < 0)
      fprintf(stderr, "statusInfo: WARNING: error initializing netlink 802.11\n");

   /* Build battery paths */
   /* TODO: not all systems report power_now, some have current_now */
   snprintf(sysfsBatteryCapacity, MX_PATH_LEN, "%s/%s/capacity", POWER_SUPPLY, BATTERY_NAME);
   snprintf(sysfsBatteryPowerNow, MX_PATH_LEN, "%s/%s/power_now", POWER_SUPPLY, BATTERY_NAME);

   fprintf(stderr, "Check path for sysfsBatteryCapacity: %s\n", getSysInfo(sysfsBatteryCapacity)==-1 ? "Failed" : "OK");
   fprintf(stderr, "Check path for sysfsBatteryPowerNow: %s\n", getSysInfo(sysfsBatteryPowerNow)==-1 ? "Failed" : "OK");

   i=getThermalPath();
   if (i>-1)
      snprintf(sysfsThermalPath, MX_PATH_LEN, "%s%i/%s", THERMAL_ZONE, i, TEMP_INPUT);

   /* Signal handler */
   ret=sigemptyset(&sigset);
   if (ret==0) {
      ret+=sigaddset(&sigset, SIGINT);
      ret+=sigaddset(&sigset, SIGTERM);
      ret+=sigaddset(&sigset, SIGHUP);
   }
   if (ret==0)
      ret=sigprocmask(SIG_SETMASK, &sigset, NULL);
   if (ret==0)
      signal_fd=signalfd(-1, &sigset, 0);
   if (signal_fd<0) {
      fprintf(stderr, "Unable to initialise signal handling\n");
      exit_request=1;
   }
   fds[1].fd=signal_fd;
   fds[1].events=POLLIN|POLLERR|POLLNVAL;
   nfd++;

   /* Initialise the mixerp struct _snd_mixer (mixer handle); O_RDONLY is for reference and is not used by snd_mixer_open()
    * mixerp is allocated and needs to be freed. Returns -ENOMEM if calloc fails, otherwise returns 0.
    */
   ret=snd_mixer_open(&mixerp, O_RDONLY);
   if (ret < 0)
      fprintf(stderr, "snd_mixer_open: %s\n", snd_strerror(ret));
   else
      ret=snd_mixer_attach(mixerp, ALSA_HW_DEVICE);  /* Adds higher level control struct _snd_hctl for default control to mixerp->slaves list in mixerp struct */

   if (ret < 0)
      fprintf(stderr, "snd_mixer_attach: %s\n", snd_strerror(ret));
   else {
      ret=snd_mixer_selem_register(mixerp, NULL, NULL);  /* For each slave in list, adds data to mixerp->classes */
      if (ret < 0)
         fprintf(stderr, "snd_mixer_selem_register: %s\n", snd_strerror(ret));
      else {
         ret=snd_mixer_load(mixerp);   /* Load and sort all mixer elements into mixerp->elems for each snd_mixer_slave_t in mixerp. */
         if (ret < 0)
            fprintf(stderr, "snd_mixer_load: %s\n", snd_strerror(ret));
         else {
            ret=-1;
            for (i=0; i<8 && si_alsaMonitor[i]!=NULL; i++) {
               //printf("%s\n", si_alsaMonitor[i]);
               snd_mixer_selem_id_alloca(&id);                    /* snd_mixer_find_selem() requires both name and id to be set */
               snd_mixer_selem_id_set_name(id, si_alsaMonitor[i]);   /* Set name of mixer simple element (char name[60];) */
               snd_mixer_selem_id_set_index(id, 0);               /* Set index of simple mixer element (unsigned int index;) */
               elem=snd_mixer_find_selem(mixerp, id);             /* Search mixerp->elems list and return an element that matches BOTH name and id */
               if (elem==NULL)
                  fprintf(stderr, "could not find mixer element %s\n", si_alsaMonitor[i]);
               else {
                  snd_mixer_elem_set_callback(elem, mixer_elem_cb);  /* Set callback function for element; sets elem->snd_mixer_elem_callback_t; must return 0 on success otherwise a negative error code */
                  ret=0;
               }
            }
         }
      }
   }

   if (ret>=0) {
      /* Get file descriptors for the mixer control. The count of descriptors is:
       * ret=1 for hw controls, corresponding to relevant device via open() or rsm_open_device()
       * ret=1 for shm controls, corresponding to aserver socket via socket(PF_LOCAL, SOCK_STREAM, 0); addr->sun_family = AF_LOCAL;
       * ret may be greater than 1 for external plugins loaded from /usr/lib/alsa-lib which return a poll descriptor array
       *    From pipewire-alsa sources, libasound_module_ctl_pipewire.so will return a single file descriptor.
       * ret=1 for remapped controls: in this case the file descriptor can change, which may make the FD returned here invalid.
       * Events in fd is set to POLLIN|POLLERR|POLLNVAL.
       */
      ret=snd_mixer_poll_descriptors_count(mixerp);
      if (ret!=1)
         fprintf(stderr, "snd_mixer_poll_descriptors: more than 1 poll descriptor: check alsa plugins. Volume events may not be reported.\n");
      ret=snd_mixer_poll_descriptors(mixerp, &fds[2], 1);
      if (ret < 0) {
         fds[2].fd=-1;
         fprintf(stderr, "snd_mixer_poll_descriptors: %s: mixer events won't be reported.\n", snd_strerror(ret));
      }
      else
         nfd++;
   }

   while(!exit_request) {
      fds[0].revents = 0;
      fds[1].revents = 0;
      fds[2].revents = 0;
      ret=poll(fds, nfd, timeout);
      if (ret <0) break;
      if (ret >0) {
         if (fds[0].revents & (POLLERR | POLLNVAL) || fds[1].revents & (POLLERR | POLLNVAL) || fds[2].revents & (POLLERR | POLLNVAL)) {
            fprintf(stderr, "Poll error\n");
            break;
         }
         sBuf[0]='\0';
         volumeLevel[0]='\0';
         /* Handle events on file desriptors */
         if (fds[0].revents & POLLIN) {
            dev=udev_monitor_receive_device(udevMon);
            if (dev != NULL) {
               udevStatus(sBuf, udevDisplayInfo, dev);
               udev_device_unref(dev);
            }
            else
               fprintf(stderr, "udev_monitor_receive_device() failed\n");
         }

         if (fds[1].revents & POLLIN) {
            if (read(signal_fd, &siginfo, sizeof(siginfo)) != sizeof(siginfo)) {
               fprintf(stderr, "Error reading signal fd\n");
            }
            break;
         }

         /* For a single FD, snd_mixer_poll_descriptors_revents() just returns (fds[2]->revents & (POLLIN|POLLERR|POLLNVAL)) */
         if (fds[2].revents & POLLIN) {
            //ret=snd_mixer_poll_descriptors_revents(mixerp, &fds[2], 1, &mixer_revents);

            /* For event type SND_CTL_EVENT_ELEM:
             *   Read events and handle certain operations (SNDRV_CTL_EVENT_MASK_REMOVE - element removed, SNDRV_CTL_EVENT_MASK_ADD - element added) internally
             *   Call callback for SNDRV_CTL_EVENT_MASK_VALUE and SNDRV_CTL_EVENT_MASK_INFO events
             * Returns: 0 if not a SND_CTL_EVENT_ELEM event or success (event handled), or callback not defined. 
             *        < 0 on error (element not found, calloc failed for new element, error adding new element)
             *          n: from user callback. NOTE: alsa-lib does not check the return value, however callback should return 0 on success otherwise a negative error code (include/mixer.h).
             */
            
            ret=snd_mixer_handle_events(mixerp);
            if (ret < 0)
               fprintf(stderr, "snd_mixer_handle_events: %s\n", snd_strerror(ret));
            else
               snprintf(sBuf, MX_STATUS_CHARS, "%s", volumeLevel);
         }

         if (sBuf[0]!='\0') {
            if (sbOut(outputFunction, &sock_address, sBuf)==-1)
               break;
            timeout=NOTIFY_TIMEOUT;
            continue;
         }
      }
      /* ret == 0: poll timed out - refresh status info */
      for (i=0; i<MX_NUMBER_ELEMENTS; i++)
         udevDisplayInfo[i][0]='\0';

      getStatusInfo(sBuf, sysfsBatteryCapacity, sysfsBatteryPowerNow, sysfsThermalPath, &nlData, &wStats);
      if (sbOut(outputFunction, &sock_address, sBuf)==-1)
         break;

      timeout=STATUS_TIMEOUT;
   }

   fprintf(stderr, "\nExit: %s received. Closing status info...\n", strsignal(siginfo.ssi_signo));
   if (signal_fd>=0)
      close(signal_fd);

   udev_monitor_unref(udevMon);
   udev_unref(udevCtx);
   if (nlData.id>=0) {
      nl_cb_put(nlData.wlanStats_cb);  /* This decreases the ref count for the cb: src: libnl: handlers.c (e.g. https://www.infradead.org/~tgr/libnl/doc/api/handlers_8c_source.html) */
      nl_close(nlData.socket);
      nl_socket_free(nlData.socket);
   }
   /* Close the mixer and free all resources */ 
   snd_mixer_close(mixerp);
   sbOut(outputFunction, &sock_address, "Status Bar Closed");
   return 0;
}

