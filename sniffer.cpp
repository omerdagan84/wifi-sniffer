const int num_channels = 12;
const float max_time = 60;
const float round_time = max_time/5;

#include "sniffer.h"
#include "protocol_headers.h"
#include "util.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <pcap.h>
#include <map>
#include <set>
#include <iostream>

using namespace std;

static pcap_t *handle = NULL;
static int datalink;
char * interface;

int current_channel = 0;

float channel_prob[num_channels+1];
float channel_time[num_channels+1];
int channel_packets[num_channels+1];

map<string,int> mac_count[num_channels+1][4];

void set_monitor_mode(char * iface) {
  interface = iface;
  char * const argv[] = {(char*)("iwconfig"),iface,(char*)("mode"),(char*)("monitor"),0};
  int ret = run_command(argv);
  if ( ret ) {
    debug("Probably on an Ubuntu system. Trying to set monitor using ifconfig technique.");
    char * const ifconfig_down[] = {(char*)("ifconfig"),iface,(char*)("down"),0};
    char * const ifconfig_up[] = {(char*)("ifconfig"),iface,(char*)("up"),0};
    ret = run_command(ifconfig_down);
    if ( ret ) {
      error("Interface error. Quitting.");
      abort();
    }
    ret = run_command(argv);
    if ( ret ) {
      error("Interface error. Quitting.");
      abort();
    }
    ret = run_command(ifconfig_up);
    if ( ret ) {
      error("Interface error. Quitting.");
      abort();
    }
  }
}

void initialize(char * interface) {
  if ( handle ) {
    error("Trying to reinitialize using interface %s",interface);
    abort();
  }

  char errbuf[BUFSIZ];

  set_monitor_mode(interface);

  handle = pcap_open_live(interface, BUFSIZ, 1, 1000, errbuf);
  if (handle == NULL) {
    error("Couldn't open interface %s. Error: %s",interface,errbuf);
    abort();
  }

  if ( pcap_setnonblock(handle,1,errbuf) == -1 ) {
    error("Couldn't set to non-blocking mode. Error: %s",errbuf);
    abort();
  }

  datalink = pcap_datalink(handle);

  verbose("Opened interface %s.",interface);
  debug("Datalink is %d.", datalink);

  for ( int i = 1 ; i <= num_channels ; i++ ) {
    channel_prob[i] = 1.0/num_channels;
    channel_time[i] = 0;
    channel_packets[i] = 0;
  }
}

void handleMAC(const u_char * mac, int pos) {
  char mac_c_str[13];
  mac_c_str[0] = 0;
  for ( int i = 0 ; i < 6 ; i++ ) {
    sprintf(mac_c_str,"%s%02X",mac_c_str,mac[i]);
  }
  string mac_str(mac_c_str);
  mac_count[current_channel][pos][mac_str]++;
  debug("MAC %d : %s",pos,mac_c_str);
}

void handlePacket(const u_char* packet, int length) {
  if ( packet == NULL ) {
    return;
  }

  if ( datalink ==	DLT_PRISM_HEADER ) {
    prism_header* rth1 = (prism_header*)(packet);
    packet = packet + rth1->msglen;
    length -= rth1->msglen;
  }

  if ( datalink == DLT_IEEE802_11_RADIO ) {
    ieee80211_radiotap_header* rth2 = (ieee80211_radiotap_header*)(packet);
    packet = packet + rth2->it_len;
    length -= rth2->it_len;
  }

  for ( int i = 0 ; i < 4 ; i++ ) {
    if ( 4+i*6 < length ) {
      handleMAC(packet+4+(i*6),i);
    }
  }

  channel_packets[current_channel]++;
}

void change_channel(int channel) {
  if ( channel < 1 || channel > num_channels ) {
    error("Impossible to switch to channel %d. Quitting.",channel);
    abort();
  }
  current_channel = channel;
  char channel_no[3];
  sprintf(channel_no,"%d",channel);
  char * const argv[] = {(char*)"iwconfig",interface,(char*)"channel",channel_no,0};
  run_command(argv);
  verbose("Changed to channel %d",channel);
}

static Timer ch_time;

void mark_time() {
  channel_time[current_channel]+=ch_time.get_time();
}

void switch_to_next_channel() {
  mark_time();
  change_channel((current_channel % num_channels) + 1);
  ch_time.reset();
}

void recalculate_probs() {
  const float min_speed_adder = 0.01;
  float speed[num_channels+1];
  float total_speed = 0;
  for ( int i = 1 ; i <= num_channels ; i++ ) {
    debug("Packets on channel %02d = %d",i,channel_packets[i]);
    speed[i] = channel_packets[i]/channel_time[i];
    speed[i] += min_speed_adder;
    total_speed += speed[i];
  }

  for ( int i = 1 ; i <= num_channels ; i++ ) {
    channel_prob[i] = speed[i] / total_speed;
  }

  verbose("Recalculated time allotted per channel (Greater time for busier channels)");
}

void callback(u_char *user,const struct pcap_pkthdr* pkthdr,const u_char* packet) {
  handlePacket(packet,pkthdr->len);
}

void capture_packets() {
  Timer timer;
  switch_to_next_channel();
  bool end_of_capturing = false;
  bool end_of_round = false;
  while ( true ) {
    if ( timer.get_time() >= max_time ) {
      end_of_capturing = true;
    }
    if ( pcap_dispatch(handle,1,callback,NULL) ) {
      debug("<<<Channel %02d timer: %f; Total timer: %f>>>",current_channel,ch_time.get_time(),timer.get_time());
    }
    if ( ch_time.get_time() > channel_prob[current_channel] * round_time ) {
      if ( current_channel==num_channels ) {
        mark_time();
        recalculate_probs();
        if ( end_of_capturing ) {
          break;
        }
      }
      switch_to_next_channel();
    }
  }
}

void print_info() {
  cout << "\n\n";
  int overall_total_mac_count = 0;
  int overall_total_packets_captured = 0;
  float overall_total_time = 0;
  set<string> overall_macs;
  for ( int i = 1 ; i <= num_channels ; i++ ) {
    if ( channel_packets[i] == 0 ) {
      continue;
    }
    int total_unique_mac_count = 0;
    int total_mac_count = 0;
    if ( is_verbose() ) {
      cout << "Channel #" << i << ":\n";
    }
    map<string,int> channel_mac_counts;
    for ( int j = 0 ; j < 4 ; j++ ) {
      for ( map<string,int>::iterator it = mac_count[i][j].begin() ; it != mac_count[i][j].end() ; it++ ) {
        channel_mac_counts[it->first] += it->second;
        overall_macs.insert(it->first);
      }
    }
    for ( map<string,int>::iterator it = channel_mac_counts.begin() ; it != channel_mac_counts.end() ; it++ ) {
      if ( is_verbose() ) {
        cout << it->first << " : " << it->second << endl;
      }
      total_mac_count += it->second;
      total_unique_mac_count += 1;
    }
    cout << "In channel " << i << ":\n";
    cout << " Number of unique MACs seen = " << total_unique_mac_count << endl;
    cout << " Total number of MACs seen  = " << total_mac_count << endl;
    cout << " Total packets captured     = " << channel_packets[i] << endl;
    cout << " Packet capture rate        = " << (channel_packets[i]/channel_time[i]) << " packets/sec" << endl;
    cout << "\n";
    overall_total_mac_count += total_mac_count;
    overall_total_packets_captured += channel_packets[i];
    overall_total_time += channel_time[i];
  }
  cout << "Overall:\n";
  cout << " Number of unique MACs seen = " << overall_macs.size() << endl;
  cout << " Total number of MACs seen  = " << overall_total_mac_count << endl;
  cout << " Total packets captured     = " << overall_total_packets_captured << endl;
  cout << " Packet capture rate        = " << overall_total_packets_captured/overall_total_time << " packets/sec" << endl;
  cout << "\n";
}

// TODO: Make sure that pcap_next doesn't take too long
// TODO: Output timetamps for each MAC in debug mode