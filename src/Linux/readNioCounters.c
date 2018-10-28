/* This software is distributed under the following license:
 * http://sflow.net/license.html
 */

#if defined(__cplusplus)
extern "C" {
#endif

#include "hsflowd.h"
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <linux/types.h>
#include <linux/ethtool.h>
#include <linux/sockios.h>

  /*_________________---------------------------__________________
    _________________ shareActorIDFromSlave     __________________
    -----------------___________________________------------------
  */

  static void shareActorIDFromSlave(HSP *sp, HSPAdaptorNIO *bond_nio, HSPAdaptorNIO *aggregator_slave_nio) {
    SFLAdaptor *adaptor;
    UTHASH_WALK(sp->adaptorsByIndex, adaptor) {
      HSPAdaptorNIO *nio = ADAPTOR_NIO(adaptor);
      if(nio->bond_slave
	 && nio != aggregator_slave_nio
	 && nio->lacp.attachedAggID == bond_nio->lacp.attachedAggID) {
	memcpy(nio->lacp.actorSystemID, aggregator_slave_nio->lacp.actorSystemID, 6);
      }
    }
  }

  /*_________________---------------------------__________________
    _________________    updateBondCounters     __________________
    -----------------___________________________------------------
  */

  void updateBondCounters(HSP *sp, SFLAdaptor *bond) {
    char procFileName[256];
    snprintf(procFileName, 256, "/proc/net/bonding/%s", bond->deviceName);
    FILE *procFile = fopen(procFileName, "r");
    if(procFile) {
      // limit the number of chars we will read from each line
      // (there can be more than this - fgets will chop for us)
#define MAX_PROC_LINE_CHARS 240
      char line[MAX_PROC_LINE_CHARS];
      SFLAdaptor *currentSlave = NULL;
      HSPAdaptorNIO *slave_nio = NULL;
      HSPAdaptorNIO *bond_nio = ADAPTOR_NIO(bond);
      HSPAdaptorNIO *aggregator_slave_nio = NULL;
      bond_nio->lacp.attachedAggID = bond->ifIndex;
      uint32_t aggID = 0;
      // make sure we don't hold on to stale data - may need
      // to pick up actorSystemID from a slave port.
      memset(bond_nio->lacp.actorSystemID, 0, 6);
      memset(bond_nio->lacp.partnerSystemID, 0, 6);
      int readingMaster = YES; // bond master data comes first
      int gotActorID = NO;
      while(fgets(line, MAX_PROC_LINE_CHARS, procFile)) {
	char buf_var[MAX_PROC_LINE_CHARS];
	char buf_val[MAX_PROC_LINE_CHARS];
	// buf_var is up to first ':', buf_val is the rest
	if(sscanf(line, "%[^:]:%[^\n]", buf_var, buf_val) == 2) {
	  char *tok_var = trimWhitespace(buf_var);
	  char *tok_val = trimWhitespace(buf_val);

	  if(readingMaster) {
	    if(my_strequal(tok_var, "MII Status")) {
	      if(my_strequal(tok_val, "up")) {
		bond_nio->lacp.portState.v.actorAdmin = 2; // dot3adAggPortActorAdminState
		bond_nio->lacp.portState.v.actorOper = 2;
		bond_nio->lacp.portState.v.partnerAdmin = 2;
		bond_nio->lacp.portState.v.partnerOper = 2;
	      }
	      else {
		bond_nio->lacp.portState.all = 0;
	      }
	    }

	    if(my_strequal(tok_var, "System Identification")) {
	      myDebug(1, "updateBondCounters: %s system identification %s",
		      bond->deviceName,
		      tok_val);
	      char sys_mac[MAX_PROC_LINE_CHARS];
	      uint64_t code;
	      if(sscanf(tok_val, "%"SCNu64"  %s", &code, sys_mac) == 2) {
		if(hexToBinary((u_char *)sys_mac,bond_nio->lacp.actorSystemID, 6) != 6) {
		  myLog(LOG_ERR, "updateBondCounters: system mac read error: %s", sys_mac);
		}
		else if(!isAllZero(bond_nio->lacp.actorSystemID, 6)) {
		  gotActorID = YES;
		}
	      }
	    }

	    if(my_strequal(tok_var, "Partner Mac Address")) {
	      myDebug(1, "updateBondCounters: %s partner mac is %s",
		      bond->deviceName,
		      tok_val);
	      if(hexToBinary((u_char *)tok_val,bond_nio->lacp.partnerSystemID, 6) != 6) {
		myLog(LOG_ERR, "updateBondCounters: partner mac read error: %s", tok_val);
	      }
	    }

	    if(my_strequal(tok_var, "Aggregator ID")) {
	      aggID = strtol(tok_val, NULL, 0);
	      myDebug(1, "updateBondCounters: %s aggID %u", bond->deviceName, aggID);
	    }
	  }

	  // initially the data is for the bond, but subsequently
	  // we get info about each slave. So we started with
	  // (readingMaster=YES,currentSlave=NULL), and now we
	  // detect transitions to slave data:
	  if(my_strequal(tok_var, "Slave Interface")) {
	    readingMaster = NO;
	    currentSlave = adaptorByName(sp, trimWhitespace(tok_val));
	    slave_nio = currentSlave ? ADAPTOR_NIO(currentSlave) : NULL;
	    myDebug(1, "updateBondCounters: bond %s slave %s %s",
		  bond->deviceName,
		  tok_val,
		  currentSlave ? "found" : "not found");
	    if(slave_nio) {
	      // initialize from bond
	      slave_nio->lacp.attachedAggID = bond->ifIndex;
	      memcpy(slave_nio->lacp.partnerSystemID, bond_nio->lacp.partnerSystemID, 6);
	      memcpy(slave_nio->lacp.actorSystemID, bond_nio->lacp.actorSystemID, 6);

	      // make sure the parent is going to export separate
	      // counters if the slave is going to (because it was
	      // marked as a switchPort):
	      if(slave_nio->switchPort) {
		if(!bond_nio->switchPort) {
		  myDebug(1, "updateBondCounters: marking bond %s as switchPort",
			  bond->deviceName);
		  bond_nio->switchPort = YES;
		}
	      }
	      // but we no longer allow the inverse to happen.  If a slave is not
	      // already marked as a switchPort then do not start treating it as one.
	      // This is not necessarily an error.  A regular server may enable traffic
	      // monitoring on a bond interface without intending to get separate
	      // counters for the components too.  Log as a warning when debugging.
	      if(bond_nio->switchPort && !slave_nio->switchPort)
		myDebug(1, "updateBondCounters: warning: bond %s slave %s not marked as switchPort",
			bond->deviceName,
			currentSlave->deviceName);
	    }
	  }

	  if(readingMaster == NO && slave_nio) {
	    if(my_strequal(tok_var, "MII Status")) {
	      if(my_strequal(tok_val, "up")) {
		slave_nio->lacp.portState.v.actorAdmin = 2; // dot3adAggPortActorAdminState
		slave_nio->lacp.portState.v.actorOper = 2;
		slave_nio->lacp.portState.v.partnerAdmin = 2;
		slave_nio->lacp.portState.v.partnerOper = 2;
	      }
	      else {
		slave_nio->lacp.portState.all = 0;
	      }
	    }

	    if(my_strequal(tok_var, "Permanent HW addr")) {
	      if(!gotActorID) {
		// Still looking for our actorSystemID, so capture this here in case we
		// decide below that it is the one we want.  Note that this mac may not be the
		// same as the mac associated with this port that we read back in readInterfaces.c.
		if(hexToBinary((u_char *)tok_val,slave_nio->lacp.actorSystemID, 6) != 6) {
		  myLog(LOG_ERR, "updateBondCounters: permanent HW addr read error: %s", tok_val);
		}
	      }
	    }

	    if(my_strequal(tok_var, "Aggregator ID")) {
	      uint32_t slave_aggID = strtol(tok_val, NULL, 0);
	      if(slave_aggID == aggID) {
		// remember that is the slave port that has the same aggregator ID as the bond
		aggregator_slave_nio = slave_nio;
	      }
	    }
	  }
	}
      }

      if(aggregator_slave_nio && !gotActorID) {
	// go back and fill in the actorSystemID on all the slave ports
	shareActorIDFromSlave(sp, bond_nio, aggregator_slave_nio);
      }

      fclose(procFile);
    }
  }

  /* ================== example of /proc/net/bonding/<if> ====================

	Ethernet Channel Bonding Driver: v3.7.1 (April 27, 2011)

	  Bonding Mode: IEEE 802.3ad Dynamic link aggregation
	  Transmit Hash Policy: layer2 (0)
	  MII Status: up
	  MII Polling Interval (ms): 100
	  Up Delay (ms): 0
	  Down Delay (ms): 0

	  802.3ad info
	  LACP rate: fast
	  Min links: 0
	  Aggregator selection policy (ad_select): stable
	  Active Aggregator Info:
	  Aggregator ID: 1
	  Number of ports: 2
	  Actor Key: 17
	  Partner Key: 17
	  Partner Mac Address: 08:9e:01:f8:9b:45

	  Slave Interface: swp3
	  MII Status: up
	  Speed: 1000 Mbps
	  Duplex: full
	  Link Failure Count: 1
	  Permanent HW addr: 08:9e:01:f8:9b:af
	  Aggregator ID: 1
	  Slave queue ID: 0

	  Slave Interface: swp4
	  MII Status: up
	  Speed: 1000 Mbps
	  Duplex: full
	  Link Failure Count: 1
	  Permanent HW addr: 08:9e:01:f8:9b:b0
	  Aggregator ID: 1
	  Slave queue ID: 0
  */

  /*_________________---------------------------__________________
    _________________    readBondState          __________________
    -----------------___________________________------------------
  */

  void readBondState(HSP *sp) {
    assert(sp);
    assert(sp->adaptorsByIndex);
    SFLAdaptor *adaptor;
    UTHASH_WALK(sp->adaptorsByIndex, adaptor) {
      assert(adaptor);
      assert(ADAPTOR_NIO(adaptor) != NULL);
      if(ADAPTOR_NIO(adaptor)->bond_master)
	updateBondCounters(sp, adaptor);
    }
  }


  /*_________________---------------------------__________________
    _________________   synthesizeBondMetaData  __________________
    -----------------___________________________------------------
  */

  static void synthesizeBondMetaData(HSP *sp, SFLAdaptor *bond) {
    uint64_t ifSpeed = 0;
    bool up = NO;
    uint32_t ifDirection = 0;
    SFLAdaptor *search_ad;

    myDebug(1, "synthesizeBondMetaData: BEFORE: bond %s (ifSpeed=%"PRIu64" dirn=%u) marked %s",
	    bond->deviceName,
	    bond->ifSpeed,
	    bond->ifDirection,
	    up ? "UP" : "DOWN");

    UTHASH_WALK(sp->adaptorsByIndex, search_ad) {
      if(search_ad != bond) {
	HSPAdaptorNIO *search_nio = ADAPTOR_NIO(search_ad);
	if(search_nio && search_nio->lacp.attachedAggID == bond->ifIndex) {

	  myDebug(1, "synthesizeBondMetaData: bond %s component %s (ifSpeed=%"PRIu64" dirn=%u up=%s)",
		  bond->deviceName,
		  search_ad->deviceName,
		  search_ad->ifSpeed,
		  search_ad->ifDirection,
		  search_nio->up ? "UP":"DOWN");

	  // sum ifSpeed
	  ifSpeed += search_ad->ifSpeed;
	  // bond is up if any slave is up
	  if(search_nio->up) up = YES;
	  // capture ifDirection -- assume the same on all components
	  if(search_ad->ifDirection) ifDirection = search_ad->ifDirection;
	}
      }
    }

    ADAPTOR_NIO(bond)->up = up;
    bond->ifSpeed = ifSpeed;
    bond->ifDirection = ifDirection;

    myDebug(1, "synthesizeBondMetaData: AFTER: bond %s (ifSpeed=%"PRIu64" dirn=%u) marked %s",
	    bond->deviceName,
	    bond->ifSpeed,
	    bond->ifDirection,
	    up ? "UP" : "DOWN");
  }

  /*_________________---------------------------__________________
    _________________    syncBondPolling        __________________
    -----------------___________________________------------------
  */

  static void syncSlavePolling(HSP *sp, SFLAdaptor *bond) {
    HSPAdaptorNIO *bond_nio = ADAPTOR_NIO(bond);
    SFLAdaptor *adaptor;
    UTHASH_WALK(sp->adaptorsByIndex, adaptor) {
      HSPAdaptorNIO *nio = ADAPTOR_NIO(adaptor);
      if(nio->bond_slave
	 && nio->lacp.attachedAggID == bond_nio->lacp.attachedAggID) {
	// put the slave on the same polling schedule as the master.
	// This isn't strictly necessary, but it will reduce the
	// frequency of access to th /proc/net/bonding file.
	if(bond_nio->poller
	   && nio->poller) {
	  myDebug(1, "sync polling so that slave %s goes with bond %s",
		  adaptor->deviceName,
		  bond->deviceName);
	  sfl_poller_synchronize_polling(nio->poller, bond_nio->poller);
	}
      }
    }
  }

  void syncBondPolling(HSP *sp) {
    SFLAdaptor *adaptor;
    UTHASH_WALK(sp->adaptorsByIndex, adaptor) {
      if(ADAPTOR_NIO(adaptor)->bond_master)
	syncSlavePolling(sp, adaptor);
    }
  }

  /*_________________---------------------------__________________
    _________________      syncPolling          __________________
    -----------------___________________________------------------
  */

  void syncPolling(HSP *sp) {
    if(sp->syncPollingInterval <= 1)
      return;
    SFLAdaptor *adaptor;
    UTHASH_WALK(sp->adaptorsByIndex, adaptor) {
      HSPAdaptorNIO *nio = ADAPTOR_NIO(adaptor);
      if(nio->poller
	 && nio->switchPort) {
	uint32_t countdown = nio->poller->countersCountdown;
	uint32_t nudgeBack = countdown % sp->syncPollingInterval;
	uint32_t nudgeFwd = sp->syncPollingInterval - nudgeBack;
	// take the smaller nudge - as long as it's in the future
	if(nudgeBack < nudgeFwd
	   && countdown > nudgeBack)
	  nio->poller->countersCountdown -= nudgeBack;
	else
	  nio->poller->countersCountdown += nudgeFwd;
      }
    }
  }

#if ( HSP_OPTICAL_STATS && ETHTOOL_GMODULEEEPROM )

  /*_________________---------------------------__________________
    _________________    SFF8472 SFP Data       __________________
    -----------------___________________________------------------
  */

  static double sff8472_calibration(double reading, uint16_t *eew, uint32_t iscale, uint32_t ioffset)
  {
    // (reading * scale) + offset
    double offset = ntohs(eew[ioffset]);
    uint16_t scale16 = ntohs(eew[iscale]);
    double scale = (double)(scale16 >> 8) + ((double)(scale16 & 0xFF) / 256.0);
    return (reading * scale) + offset;
  }

#define SFF8472_CAL(x, e, i) (x) = sff8472_calibration((x), (e), (i), (i)+1)

  static double sff8472_calibration_rxpwr(double reading, float *rxpwr)
  {
    // rxpwr[0],..,rxpwr[4] correspond to RX_PWR(4),..,RXPWR(0) in the spec
    // (i.e. in reverse order).  The calibrated result is the 16-bit sum of
    // each term multiplied by reading^N (then truncated to 16 bits)
    // i.e. RX_PWR(0) * 1
    //     +RX_PWR(1) * reading
    //     +RX_PWR(2) * reading * reading
    // and so on.
    float r = 1;
    uint16_t ans = 0;
    for(int ii = 5; --ii >= 0;) {
      ans += (uint16_t)(rxpwr[ii] * r);
      r *= reading;
    }
    return ans;
  }
#define SFF8472_CAL_RXPWR(x, ff) (x) = sff8472_calibration_rxpwr((x), (ff))

  static void sff8472_read(SFLAdaptor *adaptor, struct ifreq *ifr, int fd)
  {
    struct ethtool_eeprom *eeprom = NULL;
    HSPAdaptorNIO *nio = ADAPTOR_NIO(adaptor);

    if(nio->modinfo_len < ETH_MODULE_SFF_8472_LEN)
      goto out;

    eeprom = (struct ethtool_eeprom *)my_calloc(sizeof(*eeprom) + ETH_MODULE_SFF_8472_LEN);
    eeprom->cmd = ETHTOOL_GMODULEEEPROM;
    eeprom->len = ETH_MODULE_SFF_8472_LEN;
    ifr->ifr_data = (char *)eeprom;
    if(ioctl(fd, SIOCETHTOOL, ifr) < 0) {
      myLog(LOG_ERR, "SFF8036 ethtool ioctl failed: %s", strerror(errno));
      goto out;
    }

    if(eeprom->data[0] != 0x03 ||
       eeprom->data[1] != 0x04) {
      goto out;
    }

    // test (SFF_A0_DOM & SFF_A0_DOM_IMPL)
    if(!(eeprom->data[92] & 0x40)) {
      // no optical stats
      goto out;
    }

    uint32_t num_lanes = 1;
    uint16_t wavelength=0;
    double temperature, voltage, bias_current;
    double tx_power, tx_power_max, tx_power_min;
    double rx_power, rx_power_max, rx_power_min;

    uint16_t *eew = (uint16_t *)(eeprom->data);

    // wavelength
    if(!(eeprom->data[8] & 0x0c)) {
      wavelength = ntohs(eew[30]);
    }

    // temperature
    uint16_t temp16 = ntohs(eew[128 + 48]);
    temperature = (int8_t)(temp16 >> 8); // high byte in oC (signed)
    temperature += (double)(temp16 & 0xFF) / 256.0; // low byte in 1/256 oC

    // voltage
    voltage = ntohs(eew[128 + 49]);

    // bias current
    bias_current = ntohs(eew[128 + 50]);

    // power
    tx_power = ntohs(eew[128 + 51]);
    rx_power = ntohs(eew[128 + 52]);
    tx_power_max = ntohs(eew[128 + 12]);
    tx_power_min = ntohs(eew[128 + 13]);
    rx_power_max = ntohs(eew[128 + 16]);
    rx_power_min = ntohs(eew[128 + 17]);

    // calibration
    if(eeprom->data[92] & 0x10) {
      // apply external calibration
      SFF8472_CAL(bias_current, eew, (128 + 38));
      SFF8472_CAL(tx_power, eew, (128 + 40));
      SFF8472_CAL(tx_power_max, eew, (128 + 40));
      SFF8472_CAL(tx_power_min, eew, (128 + 40));
      SFF8472_CAL(temperature, eew, (128 + 42));
      SFF8472_CAL(voltage, eew, (128 + 44));
      // rx power calibration is a polynomial
      // read the float coefficients as uint32_t
      // so we can byte-swap them easily:
      uint32_t rxpwr[5];
      memcpy(rxpwr, eew + 128 + 28, 5 * 4);
      for(int ii = 0; ii < 5; ii++) rxpwr[ii] = ntohl(rxpwr[ii]);
      // now apply to rx_pwr
      SFF8472_CAL_RXPWR(rx_power, (float *)rxpwr);
      SFF8472_CAL_RXPWR(rx_power_min, (float *)rxpwr);
      SFF8472_CAL_RXPWR(rx_power_max, (float *)rxpwr);
    }

    // populate sFlow structure
    nio->sfp.lanes = (SFLLane *)my_realloc(nio->sfp.lanes, sizeof(SFLLane) * num_lanes);
    nio->sfp.module_id = adaptor->ifIndex;
    nio->sfp.module_total_lanes = num_lanes;
    nio->sfp.module_supply_voltage = (voltage / 10); // mV
    nio->sfp.module_temperature = (temperature * 1000); // mC
    nio->sfp.num_lanes = num_lanes;
    SFLLane *lane = &(nio->sfp.lanes[0]);
    lane->lane_index = 1;
    lane->tx_bias_current = (bias_current * 2); // uA
    lane->tx_power = (tx_power / 10); // uW
    lane->tx_power_min = (tx_power_min / 10); // uW
    lane->tx_power_max = (tx_power_max / 10); // uW
    lane->tx_wavelength = wavelength;
    lane->rx_power = (rx_power / 10); // uW
    lane->rx_power_min = (rx_power_min / 10); // uW
    lane->rx_power_max = (rx_power_max / 10); // uW
    lane->rx_wavelength = wavelength; // same as tx_wavelength

    myDebug(1, "SFP8472 %s u=%u(nm) T=%u(mC) V=%u(mV) I=%u(uA) tx=%u(uW) [%u-%u] rx=%u(uW) [%u-%u]",
	    adaptor->deviceName,
	    lane->tx_wavelength,
	    nio->sfp.module_temperature,
	    nio->sfp.module_supply_voltage,
	    lane->tx_bias_current,
	    lane->tx_power,
	    lane->tx_power_min,
	    lane->tx_power_max,
	    lane->rx_power,
	    lane->rx_power_min,
	    lane->rx_power_max);

  out:
    if(eeprom)
      my_free(eeprom);
  }

  static void sff8436_read(SFLAdaptor *adaptor, struct ifreq *ifr, int fd)
  {
    struct ethtool_eeprom *eeprom = NULL;
    HSPAdaptorNIO *nio = ADAPTOR_NIO(adaptor);

    if(nio->modinfo_len < ETH_MODULE_SFF_8436_LEN)
      goto out;

    eeprom = (struct ethtool_eeprom *)my_calloc(sizeof(*eeprom) + ETH_MODULE_SFF_8436_LEN);
    eeprom->cmd = ETHTOOL_GMODULEEEPROM;
    eeprom->len = ETH_MODULE_SFF_8436_LEN;
#ifdef HSP_TEST_QSFP
    int bytes = hexToBinary((u_char *)
			    "0d-00-02-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-1b-10-00-00-7f-92-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-ff-ff-ff-ff-ff-ff-ff-ff-00"
			    "0d-00-23-00-00-00-00-40-40-06-d5-05-69-00-00-05"
			    "0a-00-0a-00-46-49-4e-49-53-41-52-20-43-4f-52-50"
			    "20-20-20-20-07-00-90-65-46-43-42-47-34-31-30-51"
			    "42-31-43-31-30-2d-46-43-41-20-42-68-07-d0-46-db"
			    "00-01-04-da-44-53-4a-30-30-41-41-20-20-20-20-20"
			    "20-20-20-20-31-34-31-30-32-37-20-20-08-00-00-39"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "0f-10-00-a1-53-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "4b-00-fb-00-46-00-00-00-00-00-00-00-00-00-00-00"
			    "94-70-6e-f0-86-c4-7b-0c-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00"
			    "00-00-22-22-00-00-00-00-00-00-00-00-00-00-33-33"
			    "00-00-00-00-00-00-00-00-00-00-00-00-00-00-00-00",
			    &eeprom->data[0],
			    ETH_MODULE_SFF_8436_LEN);
    if(bytes != ETH_MODULE_SFF_8436_LEN) {
      myLog(LOG_ERR, "test QSFP: hexToBinary failed (bytes=%d)", bytes);
    }
#else
    ifr->ifr_data = (char *)eeprom;
    if(ioctl(fd, SIOCETHTOOL, ifr) < 0) {
      myLog(LOG_ERR, "SFF8036 ethtool ioctl failed: %s", strerror(errno));
      goto out;
    }
#endif

    // check for SFF8436_ID_DWDM_QSFP_PLUS
    if(eeprom->data[0] != 0x0d) {
      goto out;
    }

    uint32_t num_lanes = 4;
    uint16_t wavelength=0;
    double temperature, voltage, bias_current[4];
    double rx_power[4], rx_power_max, rx_power_min;

    uint16_t *eew = (uint16_t *)(eeprom->data);

    // wavelength - determined by transciever technology code
#ifndef SFF8436_DEVICE_TECH_OFFSET
    // these defs should eventually appear in ethtool.h
#define SFF8436_DEVICE_TECH_OFFSET 0x93
#define SFF8436_TRANS_TECH_MASK 0xF0
#define SFF8436_TRANS_COPPER_LNR_EQUAL (15 << 4)
#define SFF8436_TRANS_COPPER_NEAR_EQUAL (14 << 4)
#define SFF8436_TRANS_COPPER_FAR_EQUAL (13 << 4)
#define SFF8436_TRANS_COPPER_LNR_FAR_EQUAL (12 << 4)
#define SFF8436_TRANS_COPPER_PAS_EQUAL (11 << 4)
#define SFF8436_TRANS_COPPER_PAS_UNEQUAL (10 << 4)
#define SFF8436_TRANS_1490_DFB (9 << 4)
#define SFF8436_TRANS_OTHERS (8 << 4)
#define SFF8436_TRANS_1550_EML (7 << 4)
#define SFF8436_TRANS_1310_EML (6 << 4)
#define SFF8436_TRANS_1550_DFB (5 << 4)
#define SFF8436_TRANS_1310_DFB (4 << 4)
#define SFF8436_TRANS_1310_FP (3 << 4)
#define SFF8436_TRANS_1550_VCSEL (2 << 4)
#define SFF8436_TRANS_1310_VCSEL (1 << 4)
#define SFF8436_TRANS_850_VCSEL (0 << 4)
#endif

    uint8_t tx_tech = (eeprom->data[SFF8436_DEVICE_TECH_OFFSET]
		       & SFF8436_TRANS_TECH_MASK);
    switch (tx_tech) {
    case SFF8436_TRANS_850_VCSEL: wavelength = 850; break;
    case SFF8436_TRANS_1310_EML:
    case SFF8436_TRANS_1310_DFB:
    case SFF8436_TRANS_1310_FP:
    case SFF8436_TRANS_1310_VCSEL: wavelength = 1310; break;
    case SFF8436_TRANS_1550_EML:
    case SFF8436_TRANS_1550_DFB:
    case SFF8436_TRANS_1550_VCSEL: wavelength = 1550; break;
    case SFF8436_TRANS_1490_DFB: wavelength = 1490; break;
    }

    // temperature
    uint16_t temp16 = ntohs(eew[11]);
    temperature = (int8_t)(temp16 >> 8); // high byte in oC (signed)
    temperature += (double)(temp16 & 0xFF) / 256.0; // low byte in 1/256 oC

    // voltage
    voltage = ntohs(eew[13]);

    // channel stats
    for (int ch=0; ch < num_lanes; ch++) {
      rx_power[ch] = ntohs(eew[17 + ch]);
      bias_current[ch] = ntohs(eew[21 + ch]);
    }

    // power
    rx_power_max = ntohs(eew[256 + 24]);
    rx_power_min = ntohs(eew[256 + 25]);

    // populate sFlow structure
    nio->sfp.lanes = (SFLLane *)my_realloc(nio->sfp.lanes, sizeof(SFLLane) * num_lanes);
    nio->sfp.module_id = adaptor->ifIndex;
    nio->sfp.module_total_lanes = num_lanes;
    nio->sfp.module_supply_voltage = (voltage / 10); // mV
    nio->sfp.module_temperature = (temperature * 1000); // mC
    nio->sfp.num_lanes = num_lanes;

    for (int ch=0; ch < num_lanes; ch++) {
      SFLLane *lane = &(nio->sfp.lanes[ch]);
      lane->lane_index = (ch + 1);
      lane->tx_bias_current = (bias_current[ch] * 2); // uA
      lane->tx_wavelength = wavelength;
      lane->rx_power = (rx_power[ch] / 10); // uW
      lane->rx_power_min = (rx_power_min / 10); // uW
      lane->rx_power_max = (rx_power_max / 10); // uW
      lane->rx_wavelength = wavelength; // same as tx_wavelength

      myDebug(1, "SFP8436 %s[%u] u=%u(nm) T=%u(mC) V=%u(mV) I=%u(uA) tx=%u(uW) [%u-%u] rx=%u(uW) [%u-%u]",
	    adaptor->deviceName,
	    ch,
	    lane->tx_wavelength,
	    nio->sfp.module_temperature,
	    nio->sfp.module_supply_voltage,
	    lane->tx_bias_current,
	    lane->tx_power,
	    lane->tx_power_min,
	    lane->tx_power_max,
	    lane->rx_power,
	    lane->rx_power_min,
	    lane->rx_power_max);
    }

  out:
    if(eeprom)
      my_free(eeprom);
  }

#endif /* ( HSP_OPTICAL_STATS && ETHTOOL_GMODULEEEPROM ) */

  /*_________________---------------------------__________________
    _________________  accumulateNioCounters    __________________
    -----------------___________________________------------------
  */

  bool accumulateNioCounters(HSP *sp, SFLAdaptor *adaptor, SFLHost_nio_counters *ctrs, HSP_ethtool_counters *et_ctrs)
  {
    HSPAdaptorNIO *nio = ADAPTOR_NIO(adaptor);

    if(nio->bond_master
       && sp->synthesizeBondCounters) {
      // If we are synthezizing bond counters from their components, then we
      // ignore anything that we are offered for bond counters here,  but we
      // still have to iterate here to make sure that other properties such as
      // ifSpeed are rolled up correctly.
      synthesizeBondMetaData(sp, adaptor);
      return NO;
    }
    
    // have to detect discontinuities here, so use a full
    // set of latched counters and accumulators.
    bool accumulate = nio->last_update ? YES : NO;
    nio->last_update = sp->pollBus->now.tv_sec;
    uint64_t maxDeltaBytes = HSP_MAX_NIO_DELTA64;

    SFLHost_nio_counters delta;
    HSP_ethtool_counters et_delta;
#define NIO_COMPUTE_DELTA(field) delta.field = ctrs->field - nio->last_nio.field
    NIO_COMPUTE_DELTA(pkts_in);
    NIO_COMPUTE_DELTA(errs_in);
    NIO_COMPUTE_DELTA(drops_in);
    NIO_COMPUTE_DELTA(pkts_out);
    NIO_COMPUTE_DELTA(errs_out);
    NIO_COMPUTE_DELTA(drops_out);

    if(sp->nio_polling_secs == 0) {
      // 64-bit byte counters
      NIO_COMPUTE_DELTA(bytes_in);
      NIO_COMPUTE_DELTA(bytes_out);
    }
    else {
      // for case where byte counters are 32-bit,  we need
      // to use 32-bit unsigned arithmetic to avoid spikes
      delta.bytes_in = (uint32_t)ctrs->bytes_in - nio->last_bytes_in32;
      delta.bytes_out = (uint32_t)ctrs->bytes_out - nio->last_bytes_out32;
      nio->last_bytes_in32 = ctrs->bytes_in;
      nio->last_bytes_out32 = ctrs->bytes_out;
      maxDeltaBytes = HSP_MAX_NIO_DELTA32;
      // if we detect that the OS is using 64-bits then we can turn off the faster
      // NIO polling. This should probably be done based on the kernel version or some
      // other include-file definition, but it's not expensive to do it here like this:
      if(ctrs->bytes_in > 0xFFFFFFFF || ctrs->bytes_out > 0xFFFFFFFF) {
	myLog(LOG_INFO, "detected 64-bit counters - turn off faster polling");
	sp->nio_polling_secs = 0;
      }
    }

#define ET_COMPUTE_DELTA(field) et_delta.field = et_ctrs->field - nio->et_last.field
    ET_COMPUTE_DELTA(mcasts_in);
    ET_COMPUTE_DELTA(mcasts_out);
    ET_COMPUTE_DELTA(bcasts_in);
    ET_COMPUTE_DELTA(bcasts_out);

    if(accumulate) {
      // sanity check in case the counters were reset under out feet.
      // normally we leave this to the upstream collector, but these
      // numbers might be getting passed through from the hardware(?)
      // so we treat them with particular distrust.
      if(delta.bytes_in > maxDeltaBytes ||
	 delta.bytes_out > maxDeltaBytes ||
	 delta.pkts_in > HSP_MAX_NIO_DELTA32 ||
	 delta.pkts_out > HSP_MAX_NIO_DELTA32) {
	myLog(LOG_ERR, "detected counter discontinuity for %s: deltaBytes=%"PRIu64",%"PRIu64" deltaPkts=%u,%u",
	      adaptor->deviceName,
	      delta.bytes_in,
	      delta.bytes_out,
	      delta.pkts_in,
	      delta.pkts_out);

	if(debug(2)) {
	  myDebug(1, "old=[%"PRIu64",%"PRIu64",%u,%u]",
		  nio->last_nio.bytes_in,
		  nio->last_nio.bytes_out,
		  nio->last_nio.pkts_in,
		  nio->last_nio.pkts_out);
	  myDebug(1, "new=[%"PRIu64",%"PRIu64",%u,%u]",
		  ctrs->bytes_in,
		  ctrs->bytes_out,
		  ctrs->pkts_in,
		  ctrs->pkts_out);
	  myDebug(1, "logging backtrace...");
	  log_backtrace(0, NULL);
	}

	accumulate = NO;
      }
      if(et_delta.mcasts_in > HSP_MAX_NIO_DELTA64  ||
	 et_delta.mcasts_out > HSP_MAX_NIO_DELTA64 ||
	 et_delta.bcasts_in > HSP_MAX_NIO_DELTA64  ||
	 et_delta.bcasts_out > HSP_MAX_NIO_DELTA64) {
	myLog(LOG_ERR, "detected counter discontinuity in ethtool stats");
	accumulate = NO;
      }
    }

    if(accumulate) {
#define NIO_ACCUMULATE(tgt, field) (tgt)->nio.field += delta.field
      NIO_ACCUMULATE(nio, bytes_in);
      NIO_ACCUMULATE(nio, pkts_in);
      NIO_ACCUMULATE(nio, errs_in);
      NIO_ACCUMULATE(nio, drops_in);
      NIO_ACCUMULATE(nio, bytes_out);
      NIO_ACCUMULATE(nio, pkts_out);
      NIO_ACCUMULATE(nio, errs_out);
      NIO_ACCUMULATE(nio, drops_out);
#define ET_ACCUMULATE(tgt, field) (tgt)->et_total.field += et_delta.field
      ET_ACCUMULATE(nio, mcasts_in);
      ET_ACCUMULATE(nio, mcasts_out);
      ET_ACCUMULATE(nio, bcasts_in);
      ET_ACCUMULATE(nio, bcasts_out);
      
      if(nio->bond_slave
	 && sp->synthesizeBondCounters) {
	// pour these deltas into the bond totals too
	SFLAdaptor *bond = adaptorByIndex(sp, nio->lacp.attachedAggID);
	if(bond) {
	  HSPAdaptorNIO *bond_nio = ADAPTOR_NIO(bond);
	  bond_nio->last_update = sp->pollBus->now.tv_sec;
	  NIO_ACCUMULATE(bond_nio, bytes_in);
	  NIO_ACCUMULATE(bond_nio, pkts_in);
	  NIO_ACCUMULATE(bond_nio, errs_in);
	  NIO_ACCUMULATE(bond_nio, drops_in);
	  NIO_ACCUMULATE(bond_nio, bytes_out);
	  NIO_ACCUMULATE(bond_nio, pkts_out);
	  NIO_ACCUMULATE(bond_nio, errs_out);
	  NIO_ACCUMULATE(bond_nio, drops_out);

	  ET_ACCUMULATE(bond_nio, mcasts_in);
	  ET_ACCUMULATE(bond_nio, mcasts_out);
	  ET_ACCUMULATE(bond_nio, bcasts_in);
	  ET_ACCUMULATE(bond_nio, bcasts_out);
	}
      }
    }

    // latch - with struct copy
    nio->last_nio = *ctrs;
    nio->et_last = *et_ctrs;

    return accumulate;
  }

  /*_________________---------------------------__________________
    _________________    updateNioCounters      __________________
    -----------------___________________________------------------
  */

  void updateNioCounters(HSP *sp, SFLAdaptor *filter) {

    assert(EVCurrentBus() == sp->pollBus);
    time_t clk = sp->pollBus->now.tv_sec;

    // notify modules in case they want to override
    EVEventTx(sp->rootModule, EVGetEvent(sp->pollBus, HSPEVENT_UPDATE_NIO), &filter, sizeof(filter));

    if(filter == NULL) {
      // full refresh - but don't do anything if we just
      // refreshed all the numbers less than a second ago
      if (sp->nio_last_update == clk) {
	return;
      }
      sp->nio_last_update = clk;
    }
    else {
      if(ADAPTOR_NIO(filter)->last_update == clk) {
	// the requested adaptor has fresh counters
	// so nothing to do here
	return;
      }
    }

    FILE *procFile;
    procFile= fopen("/proc/net/dev", "r");
    if(procFile) {
      int fd = socket (PF_INET, SOCK_DGRAM, 0);
      struct ifreq ifr;
      memset (&ifr, 0, sizeof(ifr));
      // ASCII numbers in /proc/diskstats may be 64-bit (if not now
      // then someday), so it seems safer to read into
      // 64-bit ints with scanf first,  then copy them
      // into the host_nio structure from there.
      uint64_t bytes_in = 0;
      uint64_t pkts_in = 0;
      uint64_t errs_in = 0;
      uint64_t drops_in = 0;
      uint64_t bytes_out = 0;
      uint64_t pkts_out = 0;
      uint64_t errs_out = 0;
      uint64_t drops_out = 0;
      // limit the number of chars we will read from each line
      // (there can be more than this - fgets will chop for us)
#define MAX_PROC_LINE_CHARS 240
      char line[MAX_PROC_LINE_CHARS];
      while(fgets(line, MAX_PROC_LINE_CHARS, procFile)) {
	char deviceName[MAX_PROC_LINE_CHARS];
	// assume the format is:
	// Inter-|   Receive                                                |  Transmit
	//  face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed
	if(sscanf(line, "%[^:]:%"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64" %*u %*u %*u %*u %"SCNu64" %"SCNu64" %"SCNu64" %"SCNu64"",
		  deviceName,
		  &bytes_in,
		  &pkts_in,
		  &errs_in,
		  &drops_in,
		  &bytes_out,
		  &pkts_out,
		  &errs_out,
		  &drops_out) == 9) {
	  SFLAdaptor *adaptor = adaptorByName(sp, deviceName);
	  if(adaptor) {

	    if(filter && (filter != adaptor))
	      continue;

	    HSPAdaptorNIO *niostate = ADAPTOR_NIO(adaptor);

	    if(niostate->procNetDev == NO)
	      continue;

	    SFLHost_nio_counters ctrs = {
	      .bytes_in = bytes_in,
	      .pkts_in = (uint32_t)pkts_in,
	      .errs_in = (uint32_t)errs_in,
	      .drops_in = (uint32_t)drops_in,
	      .bytes_out = bytes_out,
	      .pkts_out = (uint32_t)pkts_out,
	      .errs_out = (uint32_t)errs_out,
	      .drops_out = (uint32_t)drops_out
	    };
	    HSP_ethtool_counters et_ctrs = { 0 };
	    if (niostate->ethtool_GSTATS
		&& niostate->et_found) {
	      // get the latest stats block for this device via ethtool
	      // and read out the counters that we located by name.

	      uint32_t bytes = sizeof(struct ethtool_stats);
	      bytes += niostate->et_nctrs * sizeof(uint64_t);
	      bytes += 32; // pad - just in case driver wants to write more
	      struct ethtool_stats *et_stats = (struct ethtool_stats *)my_calloc(bytes);
	      et_stats->cmd = ETHTOOL_GSTATS;
	      et_stats->n_stats = niostate->et_nctrs;

	      // now issue the ioctl
	      strncpy(ifr.ifr_name, adaptor->deviceName, sizeof(ifr.ifr_name)-1);
	      ifr.ifr_data = (char *)et_stats;
	      if(ioctl(fd, SIOCETHTOOL, &ifr) >= 0) {
		if(getDebug() > 2) {
		  for(int xx = 0; xx < et_stats->n_stats; xx++) {
		    myDebug(1, "ethtool counter for %s at index %d == %"PRIu64,
			    adaptor->deviceName,
			    xx,
			    et_stats->data[xx]);
		  }
		}
		if(niostate->et_idx_mcasts_in)
		  et_ctrs.mcasts_in = et_stats->data[niostate->et_idx_mcasts_in - 1];
		if(niostate->et_idx_mcasts_out)
		  et_ctrs.mcasts_out = et_stats->data[niostate->et_idx_mcasts_out - 1];
		if(niostate->et_idx_bcasts_in)
		  et_ctrs.bcasts_in = et_stats->data[niostate->et_idx_bcasts_in - 1];
		if(niostate->et_idx_bcasts_out)
		  et_ctrs.bcasts_out = et_stats->data[niostate->et_idx_bcasts_out - 1];
	      }
	      my_free(et_stats);
	    }

#if ( HSP_OPTICAL_STATS && ETHTOOL_GMODULEEEPROM )
	    if(filter) {
	      // If we are refreshing stats for an individual device, then
	      // check for SFP (lane) stats too. This operation can be slow so
	      // it's important to avoid doing it when we are refreshing
	      // counters for all interfaces for host-sflow network totals.
	      // Since the host-sflow network totals do not include optical
	      // stats,  this is not a problem.
	      switch(niostate->modinfo_type) {
	      case ETH_MODULE_SFF_8472: sff8472_read(adaptor, &ifr, fd); break;
	      case ETH_MODULE_SFF_8436: sff8436_read(adaptor, &ifr, fd); break;
	      }
	    }
#endif /*  ( HSP_OPTICAL_STATS && ETHTOOL_GMODULEEEPROM ) */

	    accumulateNioCounters(sp, adaptor, &ctrs, &et_ctrs);
	  }
	}
      }
      if(fd >= 0)
	close(fd);
      fclose(procFile);
    }
  }

  /*_________________---------------------------__________________
    _________________      readNioCounters      __________________
    -----------------___________________________------------------
  */

  int readNioCounters(HSP *sp, SFLHost_nio_counters *nio, char *devFilter, SFLAdaptorList *adList) {
    int interface_count = 0;
    size_t devFilterLen = devFilter ? strlen(devFilter) : 0;

    // may need to schedule intermediate calls to updateNioCounters()
    // too (to avoid undetected wraps), but at the very least we need to do
    // it here to make sure the data is up to the second.
    updateNioCounters(sp, NULL);

    SFLAdaptor *adaptor;
    UTHASH_WALK(sp->adaptorsByName, adaptor) {
      // note that the devFilter here is a prefix-match
      if(devFilter == NULL || !strncmp(devFilter, adaptor->deviceName, devFilterLen)) {
	if(adList == NULL || adaptorListGet(adList, adaptor->deviceName) != NULL) {
	  HSPAdaptorNIO *niostate = ADAPTOR_NIO(adaptor);

	  // in the case where we are adding up across all
	  // interfaces, be careful to avoid double-counting.
	  // By leaving this test until now we make it possible
	  // to know the counters for any interface or sub-interface
	  // if required (e.g. for the readPackets() module).
	  if(devFilter == NULL && (niostate->up == NO
				   || niostate->vlan != HSP_VLAN_ALL
				   || niostate->loopback
				   || niostate->bond_master)) {
	    continue;
	  }

	  interface_count++;
	  // report the sum over all devices that match the filter
	  nio->bytes_in += niostate->nio.bytes_in;
	  nio->pkts_in += niostate->nio.pkts_in;
	  nio->errs_in += niostate->nio.errs_in;
	  nio->drops_in += niostate->nio.drops_in;
	  nio->bytes_out += niostate->nio.bytes_out;
	  nio->pkts_out += niostate->nio.pkts_out;
	  nio->errs_out += niostate->nio.errs_out;
	  nio->drops_out += niostate->nio.drops_out;
	}
      }
    }
    return interface_count;
  }

#if defined(__cplusplus)
} /* extern "C" */
#endif
