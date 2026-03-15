#include "hud.h"

#include <dbus/dbus.h>
#include <dbus-c++/dbus.h>
#include <stdint.h>
#include <string>
#include <functional>
#include <condition_variable>
#include <signal.h>
#include <thread>

#include "../dbus/generated_cmu.h"

#define LOGTAG "mazda-hud"

#include "hu_uti.h"

#define SERVICE_BUS_ADDRESS "unix:path=/tmp/dbus_service_socket"
#define HMI_BUS_ADDRESS "unix:path=/tmp/dbus_hmi_socket"

std::atomic<uint32_t> hud_seq{0};
std::condition_variable hud_cv;
std::mutex hud_cv_mutex;
static std::atomic<bool> hud_active{false};

static HUDSettingsClient *hud_client = NULL;
static NaviClient *vbsnavi_client = NULL;
static TMCClient *tmc_client = NULL;

NaviData *navi_data = NULL;

uint8_t turns[][3] = {
  {0,0,0}, //TURN_UNKNOWN
  {NaviTurns::FLAG_LEFT,NaviTurns::FLAG_RIGHT,NaviTurns::FLAG}, //TURN_DEPART
  {NaviTurns::STRAIGHT,NaviTurns::STRAIGHT,NaviTurns::STRAIGHT}, //TURN_NAME_CHANGE
  {NaviTurns::SLIGHT_LEFT,NaviTurns::SLIGHT_RIGHT,NaviTurns::STRAIGHT}, //TURN_SLIGHT_TURN
  {NaviTurns::LEFT,NaviTurns::RIGHT,0}, //TURN_TURN
  {NaviTurns::SHARP_LEFT,NaviTurns::SHARP_RIGHT,0}, //TURN_SHARP_TURN
  {NaviTurns::U_TURN_LEFT, NaviTurns::U_TURN_RIGHT,0}, //TURN_U_TURN
  {NaviTurns::LEFT,NaviTurns::RIGHT,NaviTurns::STRAIGHT}, //TURN_ON_RAMP
  {NaviTurns::OFF_RAMP_LEFT,NaviTurns::OFF_RAMP_RIGHT,NaviTurns::STRAIGHT}, //TURN_OFF_RAMP
  {NaviTurns::FORK_LEFT, NaviTurns::FORK_RIGHT, 0}, //TURN_FORK
  {NaviTurns::MERGE_LEFT, NaviTurns::MERGE_RIGHT, 0}, //TURN_MERGE
  {0,0,0},  //TURN_ROUNDABOUT_ENTER
  {0,0,0}, // TURN_ROUNDABOUT_EXIT
  {0,0,0}, //TURN_ROUNDABOUT_ENTER_AND_EXIT (Will have to handle seperatly)
  {NaviTurns::STRAIGHT,NaviTurns::STRAIGHT,NaviTurns::STRAIGHT}, //TURN_STRAIGHT
  {NaviTurns::NO_CAR, NaviTurns::NO_CAR, NaviTurns::NO_CAR}, //unused?
  {0,0,0}, //TURN_FERRY_BOAT
  {0,0,0}, //TURN_FERRY_TRAIN
  {NaviTurns::CAR, NaviTurns::CAR, NaviTurns::CAR}, //unused??
  {NaviTurns::DESTINATION_LEFT, NaviTurns::DESTINATION_RIGHT, NaviTurns::DESTINATION} //TURN_DESTINATION
};

uint8_t roundabout(int32_t degrees, int32_t side){
  uint8_t nearest = (degrees + 15) / 30;
  uint8_t offset = side == 0 ? 49 : 37;
  return(nearest + offset);
}

void hud_thread_func(){
  std::this_thread::sleep_for(std::chrono::milliseconds(1000));
  NaviData prev = {};
  //Don't bother with the HUD if we aren't connected via dbus
  while (hud_active.load(std::memory_order_relaxed) && hud_installed())
  {
    // Seqlock read
    NaviData snapshot;
    uint32_t s1, s2;
    do {
      s1 = hud_seq.load(std::memory_order_acquire);
      if (s1 & 1) continue;
      std::memcpy(&snapshot, navi_data, sizeof(NaviData));
      std::atomic_thread_fence(std::memory_order_acquire);
      s2 = hud_seq.load(std::memory_order_relaxed);
    } while (s1 != s2);

    // Determine what changed by comparing with previous snapshot
    bool event_changed = (snapshot.sync_bit != prev.sync_bit);
    bool distance_changed = (snapshot.distance != prev.distance ||
                             snapshot.distance_unit != prev.distance_unit ||
                             snapshot.time_until != prev.time_until ||
                             snapshot.turn_angle != prev.turn_angle ||
                             snapshot.turn_side != prev.turn_side);

    uint8_t setGuidsncePointInfo = event_changed || distance_changed;
    ::DBus::Struct< uint32_t, uint16_t, uint8_t, uint16_t, uint8_t, uint8_t > hudDisplayMsg;
    if (setGuidsncePointInfo) {
      uint32_t diricon;
      if (snapshot.turn_event == 13) {
        diricon = roundabout(snapshot.turn_angle, snapshot.turn_side - 1);
      } else {
        int32_t turn_side = snapshot.turn_side - 1; //Google starts at 1 for some reason...
        diricon = turns[snapshot.turn_event][turn_side];
      }

      hudDisplayMsg._1 = diricon;
      hudDisplayMsg._2 = snapshot.distance;// distance;
      hudDisplayMsg._3 = snapshot.distance_unit;
      hudDisplayMsg._4 = 0; //Speed limit (Not Used)
      hudDisplayMsg._5 = 0; //Speed limit units (Not used)
      hudDisplayMsg._6 = snapshot.sync_bit;
    }
    
    uint8_t setGuidncePointName = event_changed;
    ::DBus::Struct< std::string, uint8_t > guidancePointData;
    if (setGuidncePointName) {
      guidancePointData._1 = snapshot.event_name;
      guidancePointData._2 = snapshot.sync_bit;
    }

    prev = snapshot;
    try
    {
      if (setGuidsncePointInfo) {
        vbsnavi_client->SetHUDDisplayMsgReq(hudDisplayMsg);
      }
      if (setGuidncePointName) {
        tmc_client->SetHUD_Display_Msg2(guidancePointData);
      }
    }
    catch(DBus::Error& error)
    {
      loge("DBUS: hud_send failed %s: %s\n", error.name(), error.message());
      return;
    }

    {
      std::unique_lock<std::mutex> lk(hud_cv_mutex);
      hud_cv.wait(lk);
    }
  }
}

void hud_start()
{
  if (hud_client != NULL)
    return;
    
  try
  {
    DBus::Connection service_bus(SERVICE_BUS_ADDRESS, false);
    service_bus.register_bus();
    DBus::Connection hmiBus(HMI_BUS_ADDRESS, false);
    hmiBus.register_bus();
    hud_client = new HUDSettingsClient(hmiBus, "/com/jci/navi2IHU", "com.jci.navi2IHU");
    vbsnavi_client = new NaviClient(service_bus, "/com/jci/vbs/navi", "com.jci.vbs.navi");
    tmc_client = new TMCClient(service_bus, "/com/jci/vbs/navi", "com.jci.vbs.navi");
  }
  catch(DBus::Error& error)
  {
    loge("DBUS: Failed to connect to SERVICE bus %s: %s\n", error.name(), error.message());
    hud_stop();
    return;
  }
  //logv("HUD dbus connections established\n");
  navi_data = new NaviData();
  hud_active.store(true, std::memory_order_relaxed);
  return;
}

void hud_stop()
{
  hud_active.store(false, std::memory_order_relaxed);

  delete hud_client;
  hud_client = nullptr;

  delete vbsnavi_client;
  vbsnavi_client = nullptr;

  delete tmc_client;
  tmc_client = nullptr;
}

void hud_request_stop()
{
  hud_active.store(false, std::memory_order_relaxed);
  hud_cv.notify_one();
}

bool hud_installed()
{
  if (hud_client == NULL)
      return(false);

  try
  {
      return(hud_client->GetHUDIsInstalled());
  }
  catch(DBus::Error& error)
  {
      loge("DBUS: GetHUDIsInstalled failed %s: %s\n", error.name(), error.message());
      return(false);
  }
}
