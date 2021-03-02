/********************************************************************
* Программа, которая позволяет прочитать некоторую информацию       *
* при помощи интерфейса nl80211                                     *
********************************************************************/

#define _XOPEN_SOURCE 700 //включает определения некоторых дополнительных функций, которые определены в стандартах X / Open и POSIX (в текущем случае X / Open 7, включая POSIX 2008)
#include <stdio.h>
#include <signal.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <netlink/netlink.h>    // определяет функции библиотеки netlink
#include <netlink/genl/genl.h>  // определяет функции genl_connect и genlmsg_put
#include <netlink/genl/family.h>
#include <netlink/genl/ctrl.h>  // определяет функцию genl_ctrl_resolve
#include <linux/nl80211.h>      // определяет функции и difinе'ы интерфейса NL80211


static volatile int keepRunning = 1;

void ctrl_c_handler(int dummy) {
    keepRunning = 0;
}


typedef struct {           // объявляем структуру именем Netlink
  int id;
  struct nl_sock* socket;
  struct nl_cb* cb1,* cb2;
  int result1, result2;
} Netlink;

typedef struct {           // объявляем структуру именем Wifi
  char ifname[30];
  int ifindex;
  int signal;
  int txrate;
} Wifi;

static struct nla_policy stats_policy[NL80211_STA_INFO_MAX + 1] = {
  [NL80211_STA_INFO_INACTIVE_TIME] = { .type = NLA_U32 },
  [NL80211_STA_INFO_RX_BYTES] = { .type = NLA_U32 },
  [NL80211_STA_INFO_TX_BYTES] = { .type = NLA_U32 },
  [NL80211_STA_INFO_RX_PACKETS] = { .type = NLA_U32 },
  [NL80211_STA_INFO_TX_PACKETS] = { .type = NLA_U32 },
  [NL80211_STA_INFO_SIGNAL] = { .type = NLA_U8 },
  [NL80211_STA_INFO_TX_BITRATE] = { .type = NLA_NESTED },
  [NL80211_STA_INFO_LLID] = { .type = NLA_U16 },
  [NL80211_STA_INFO_PLID] = { .type = NLA_U16 },
  [NL80211_STA_INFO_PLINK_STATE] = { .type = NLA_U8 },
};

static struct nla_policy rate_policy[NL80211_RATE_INFO_MAX + 1] = {
  [NL80211_RATE_INFO_BITRATE] = { .type = NLA_U16 },
  [NL80211_RATE_INFO_MCS] = { .type = NLA_U8 },
  [NL80211_RATE_INFO_40_MHZ_WIDTH] = { .type = NLA_FLAG },
  [NL80211_RATE_INFO_SHORT_GI] = { .type = NLA_FLAG },
};


static int initNl80211(Netlink* nl, Wifi* w);                    // функция инициализирует связь с ядром
static int finish_handler(struct nl_msg *msg, void *arg);
static int getWifiName_callback(struct nl_msg *msg, void *arg);
static int getWifiInfo_callback(struct nl_msg *msg, void *arg);
static int getWifiStatus(Netlink* nl, Wifi* w);


static int initNl80211(Netlink* nl, Wifi* w) {
  nl->socket = nl_socket_alloc();                                // выделяем сокет netlink
  if (!nl->socket) {
    fprintf(stderr, "Failed to allocate netlink socket.\n");
    return -ENOMEM;
  }

  nl_socket_set_buffer_size(nl->socket, 8192, 8192);             //устанавливаем размер буфера сокета

  if (genl_connect(nl->socket)) {                                //при помощи функции genl_connect мы подключаемся к сокету netlink
    fprintf(stderr, "Failed to connect to netlink socket.\n");
    nl_close(nl->socket);
    nl_socket_free(nl->socket);
    return -ENOLINK;
  }

  nl->id = genl_ctrl_resolve(nl->socket, "nl80211");             //просим ядро преобразовать имя nl80211 в id (семейный идентификатор)
  if (nl->id< 0) {
    fprintf(stderr, "Nl80211 interface not found.\n");
    nl_close(nl->socket);
    nl_socket_free(nl->socket);
    return -ENOENT;
  }

  nl->cb1 = nl_cb_alloc(NL_CB_DEFAULT);                          //при помощи функции nl_cb_alloc мы выделяем
  nl->cb2 = nl_cb_alloc(NL_CB_DEFAULT);                          //два новых дескриптора обрытного вызова
  if ((!nl->cb1) || (!nl->cb2)) {
     fprintf(stderr, "Failed to allocate netlink callback.\n");
     nl_close(nl->socket);
     nl_socket_free(nl->socket);
     return -ENOMEM;
  }

  nl_cb_set(nl->cb1, NL_CB_VALID , NL_CB_CUSTOM, getWifiName_callback, w);          //при помощи функции nl_cb_set
  nl_cb_set(nl->cb1, NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, &(nl->result1));   //устанавливаем следующие
  nl_cb_set(nl->cb2, NL_CB_VALID , NL_CB_CUSTOM, getWifiInfo_callback, w);          //обратные вызовы
  nl_cb_set(nl->cb2, NL_CB_FINISH, NL_CB_CUSTOM, finish_handler, &(nl->result2));
  return nl->id;
}


static int finish_handler(struct nl_msg *msg, void *arg) {        //функция finish_handler позволит нам получать сообщения от ядра
  int *ret = arg;
  *ret = 0;
  return NL_SKIP;
}


static int getWifiName_callback(struct nl_msg *msg, void *arg) { //функция, которая анализирует сообщение от ядра и получает имя интерфейса (ifname) и его индекс (ifindex).
 
  struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));

  struct nlattr *tb_msg[NL80211_ATTR_MAX + 1];

  nla_parse(tb_msg,
            NL80211_ATTR_MAX,
            genlmsg_attrdata(gnlh, 0),
            genlmsg_attrlen(gnlh, 0),
            NULL);

  if (tb_msg[NL80211_ATTR_IFNAME]) {
    strcpy(((Wifi*)arg)->ifname, nla_get_string(tb_msg[NL80211_ATTR_IFNAME]));
  }

  if (tb_msg[NL80211_ATTR_IFINDEX]) {
    ((Wifi*)arg)->ifindex = nla_get_u32(tb_msg[NL80211_ATTR_IFINDEX]);
  }

  return NL_SKIP;
}


static int getWifiInfo_callback(struct nl_msg *msg, void *arg) { //функция анализирует сообщение от ядра и получаетсигнал Wi-Fi (wifi_signal) и tx rate (канальная скорость передачи по WiFi) (wifi_bitrate).  
  struct nlattr *tb[NL80211_ATTR_MAX + 1];
  struct genlmsghdr *gnlh = nlmsg_data(nlmsg_hdr(msg));
  struct nlattr *sinfo[NL80211_STA_INFO_MAX + 1];
  struct nlattr *rinfo[NL80211_RATE_INFO_MAX + 1];
  //nl_msg_dump(msg, stdout);

  nla_parse(tb,
            NL80211_ATTR_MAX,
            genlmsg_attrdata(gnlh, 0),
            genlmsg_attrlen(gnlh, 0),
            NULL);

  if (!tb[NL80211_ATTR_STA_INFO]) {
    fprintf(stderr, "sta stats missing!\n"); return NL_SKIP;
  }

  if (nla_parse_nested(sinfo, NL80211_STA_INFO_MAX,
                       tb[NL80211_ATTR_STA_INFO], stats_policy)) {
    fprintf(stderr, "failed to parse nested attributes!\n"); return NL_SKIP;
  }

  if (sinfo[NL80211_STA_INFO_SIGNAL]) {
    ((Wifi*)arg)->signal = 100+(int8_t)nla_get_u8(sinfo[NL80211_STA_INFO_SIGNAL]);
  }

  if (sinfo[NL80211_STA_INFO_TX_BITRATE]) {
    if (nla_parse_nested(rinfo, NL80211_RATE_INFO_MAX,
                         sinfo[NL80211_STA_INFO_TX_BITRATE], rate_policy)) {
      fprintf(stderr, "failed to parse nested rate attributes!\n"); }
    else {
      if (rinfo[NL80211_RATE_INFO_BITRATE]) {
        ((Wifi*)arg)->txrate = nla_get_u16(rinfo[NL80211_RATE_INFO_BITRATE]);
      }
    }
  }
  return NL_SKIP;
}


static int getWifiStatus(Netlink* nl, Wifi* w) {     //функция, которая позволяет получить необходимую информацию устройства
  nl->result1 = 1;
  nl->result2 = 1;

  struct nl_msg* msg1 = nlmsg_alloc();  // выделяем структуру сообщения netlink при помощи функции nlmsg_alloc
  if (!msg1) {
    fprintf(stderr, "Failed to allocate netlink message.\n");
    return -2;
  }

  genlmsg_put(msg1,                 // шаг 1. цель: получение имени и индекса беспроводного интерфейса
              NL_AUTO_PORT,         // при помощи функции genlmsg_put добавляем общие загаловки netlink  в сообщение netlink
              NL_AUTO_SEQ,
              nl->id,
              0,
              NLM_F_DUMP,
              NL80211_CMD_GET_INTERFACE, //используем идентификатор команды NL80211_CMD_GET_INTERFACE для получения имени и индекса беспроводного интерфейса
              0);

  nl_send_auto(nl->socket, msg1);  // при помощи функции nl_send_auto передаем сообщение netlink

  while (nl->result1 > 0) { nl_recvmsgs(nl->socket, nl->cb1); }  //при помощи функции nl_recvmsgs получаем набор сообщений из сокета netlink
  nlmsg_free(msg1);  // функция nlmsg_free освоождает сообщение msg1 

  if (w->ifindex < 0) { return -1; }

  struct nl_msg* msg2 = nlmsg_alloc();

  if (!msg2) {
    fprintf(stderr, "Failed to allocate netlink message.\n");
    return -2;
  }

  genlmsg_put(msg2,                    // шаг 2. цель: получить мощность сигнала и скорость передачи битов
              NL_AUTO_PORT,
              NL_AUTO_SEQ,
              nl->id,
              0,
              NLM_F_DUMP,
              NL80211_CMD_GET_STATION, //используем идентификатор команды NL80211_CMD_GET_STATION, чтобы получить мощность сигнала и скорость передачи битов
              0);

  nla_put_u32(msg2, NL80211_ATTR_IFINDEX, w->ifindex); //указываем в сообщении индекс интерфейса, используя nla_put_u32
  nl_send_auto(nl->socket, msg2);
  while (nl->result2 > 0) { nl_recvmsgs(nl->socket, nl->cb2); }
  nlmsg_free(msg2);

  return 0;
}


int main(int argc, char **argv) {
  Netlink nl;
  Wifi w;

  signal(SIGINT, ctrl_c_handler);

  nl.id = initNl80211(&nl, &w);  // вызываем функцию initNl80211, которая инициализурует связь
  if (nl.id < 0) {
    fprintf(stderr, "Error initializing netlink 802.11\n");
    return -1;
  }

  do {                       // запускаем "бесконечный" цикл (до закрытия приложения), который вызывает функцию getWifiStatus() каждую 1 секунду 
    getWifiStatus(&nl, &w);
    printf("Interface: %s | signal: %d dB | txrate: %.1f MBit/s\n",
           w.ifname, w.signal, (float)w.txrate/10);
    sleep(1);
  } while(keepRunning);

  printf("\nExiting gracefully... ");
  nl_cb_put(nl.cb1);
  nl_cb_put(nl.cb2);
  nl_close(nl.socket);
  nl_socket_free(nl.socket);
  printf("OK\n");
  return 0;
}
