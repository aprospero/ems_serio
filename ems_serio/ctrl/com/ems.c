#include <stdio.h>
#include <arpa/inet.h>
#include <string.h>

#include "ems.h"
#include "../../tx.h"
#include "../../msg_queue.h"
#include "tool/logger.h"
#include "ctrl/com/mqtt.h"

struct ems_plus_t01a5       emsplus_t01a5;
struct ems_uba_monitor_fast uba_mon_fast;
struct ems_uba_monitor_slow uba_mon_slow;
struct ems_uba_monitor_wwm  uba_mon_wwm;

struct entity_params
{
  size_t   offs;
  size_t   size;
  uint32_t mask;
  const char * fmt;
  const char * type;
  const char * entity;
  time_t       last_publish;
};


struct entity_params uba_mon_fast_params[] =
{
  { offsetof(struct ems_uba_monitor_fast, tmp.water   ), sizeof(uba_mon_fast.tmp.water   ), 0xFFFFFFFF, "%d",   "sensor", "uba_water"        },
  { offsetof(struct ems_uba_monitor_fast, vl_ist      ), sizeof(uba_mon_fast.vl_ist      ), 0xFFFFFFFF, "%d",   "sensor", "uba_vl_act"       },
  { offsetof(struct ems_uba_monitor_fast, vl_soll     ), sizeof(uba_mon_fast.vl_soll     ), 0xFFFFFFFF, "%u",   "sensor", "uba_vl_nom"       },
  { offsetof(struct ems_uba_monitor_fast, on          ), sizeof(uba_mon_fast.on          ), 0x00000020, "%u",   "relay" , "uba_on_pump"      },
  { offsetof(struct ems_uba_monitor_fast, on          ), sizeof(uba_mon_fast.on          ), 0x00000001, "%u",   "relay" , "uba_on_gas"       },
  { offsetof(struct ems_uba_monitor_fast, on          ), sizeof(uba_mon_fast.on          ), 0x00000040, "%u",   "relay" , "uba_on_valve"     },
  { offsetof(struct ems_uba_monitor_fast, on          ), sizeof(uba_mon_fast.on          ), 0x00000004, "%u",   "relay" , "uba_on_blower"    },
  { offsetof(struct ems_uba_monitor_fast, on          ), sizeof(uba_mon_fast.on          ), 0x00000080, "%u",   "relay" , "uba_on_circ"      },
  { offsetof(struct ems_uba_monitor_fast, on          ), sizeof(uba_mon_fast.on          ), 0x00000008, "%u",   "relay" , "uba_on_igniter"   },
  { offsetof(struct ems_uba_monitor_fast, ks_akt_p    ), sizeof(uba_mon_fast.ks_akt_p    ), 0xFFFFFFFF, "%u",   "sensor", "uba_power_act"    },
  { offsetof(struct ems_uba_monitor_fast, ks_max_p    ), sizeof(uba_mon_fast.ks_max_p    ), 0xFFFFFFFF, "%u",   "sensor", "uba_power_max"    },
  { offsetof(struct ems_uba_monitor_fast, fl_current  ), sizeof(uba_mon_fast.fl_current  ), 0xFFFFFFFF, "%u",   "sensor", "uba_flame_current"},
  { offsetof(struct ems_uba_monitor_fast, err         ), sizeof(uba_mon_fast.err         ), 0xFFFFFFFF, "%u",   "sensor", "uba_error"        },
  { offsetof(struct ems_uba_monitor_fast, service_code), sizeof(uba_mon_fast.service_code), 0xFFFFFFFF, "%.2s", "sensor", "uba_service_code" }
};

struct entity_params uba_mon_slow_params[] =
{
  { offsetof(struct ems_uba_monitor_slow, tmp_out)              , sizeof(uba_mon_slow.tmp_out)              , 0xFFFFFFFF, "%d",   "sensor", "uba_water"     },
  { offsetof(struct ems_uba_monitor_slow, run_time)             , sizeof(uba_mon_slow.run_time)             , 0xFFFFFFFF, "%d",   "sensor", "uba_vl_act"    },
  { offsetof(struct ems_uba_monitor_slow, pump_mod)             , sizeof(uba_mon_slow.pump_mod)             , 0xFFFFFFFF, "%u",   "sensor", "uba_vl_nom"    },
  { offsetof(struct ems_uba_monitor_slow, run_time_heating_sane), sizeof(uba_mon_slow.run_time_heating_sane), 0xFFFFFFFF, "%u",   "relay" , "uba_on_pump"   },
  { offsetof(struct ems_uba_monitor_slow, run_time_stage_2_sane), sizeof(uba_mon_slow.run_time_stage_2_sane), 0xFFFFFFFF, "%u",   "relay" , "uba_on_gas"    },
  { offsetof(struct ems_uba_monitor_slow, burner_starts_sane   ), sizeof(uba_mon_slow.burner_starts_sane)   , 0xFFFFFFFF, "%u",   "relay" , "uba_on_valve"  }
};


#define SWAP_TEL_S(MSG,MEMBER,OFFS,LEN) { if (offsetof(typeof(MSG),MEMBER) >= OFFS && offsetof(typeof(MSG),MEMBER) + sizeof((MSG).MEMBER) - 1 <= ((OFFS) + (LEN))) (MSG).MEMBER = ntohs((MSG).MEMBER); } while (0)
#define CHECK_PUB(MSG,MEMBER,TYPE,ENTITY,OFFS,LEN) { if (offsetof(typeof(MSG),MEMBER) >= OFFS && offsetof(typeof(MSG),MEMBER) + sizeof((MSG).MEMBER) - 1 <= ((OFFS) + (LEN))) mqtt_publish(mqtt, TYPE, ENTITY, (MSG).MEMBER); } while (0)
#define CHECK_PUB_TRIVAL(MSG,MEMBER,TYPE,ENTITY,OFFS,LEN) { if (offsetof(typeof(MSG),MEMBER) >= OFFS && offsetof(typeof(MSG),MEMBER) + sizeof((MSG).MEMBER) - 1 <= ((OFFS) + (LEN))) mqtt_publish(mqtt, TYPE, ENTITY, ((MSG).MEMBER)[0] + (((MSG).MEMBER)[1] << 8) + (((MSG).MEMBER)[0] << 16)); } while (0)
#define CHECK_PUB_FLG(MSG,MEMBER,FLAG,TYPE,ENTITY,OFFS,LEN) { if (offsetof(typeof(MSG),MEMBER) >= OFFS && offsetof(typeof(MSG),MEMBER) + sizeof((MSG).MEMBER) - 1 <= ((OFFS) + (LEN))) mqtt_publish(mqtt, TYPE, ENTITY, (MSG).MEMBER.FLAG); } while (0)
#define CHECK_PUB_FORMATTED(MSG,MEMBER,TYPE,ENTITY,FORMAT,OFFS,LEN) { if (offsetof(typeof(MSG),MEMBER) >= OFFS && offsetof(typeof(MSG),MEMBER) + sizeof((MSG).MEMBER) - 1 <= ((OFFS) + (LEN))) mqtt_publish_formatted(mqtt, TYPE, ENTITY, FORMAT, (MSG).MEMBER); } while (0)

#define HTONU_TRIVAL(VALUE) (VALUE[0] + (VALUE[1] << 8) + (VALUE[2] << 16))
#define HTON_TRIVAL(VALUE)  (VALUE[0] + (VALUE[1] << 8) + (VALUE[2] << 16) + ((VALUE[2] >> 7) * 0xFF000000UL))



void ems_swap_telegram(struct ems_telegram * tel, size_t len)
{
  len -= 5;
  switch (tel->h.type)
  {
    case ETT_EMSPLUS:
    {
      tel->d.emsplus.type = ntohs(tel->d.emsplus.type);
      switch (tel->d.emsplus.type)
      {
        case EMSPLUS_01A5:
          memcpy(((uint8_t *) &emsplus_t01a5) + tel->h.offs, tel->d.emsplus.d.raw, len - sizeof(tel->d.emsplus.type));
          SWAP_TEL_S(emsplus_t01a5, room_temp_act, tel->h.offs, len);
          SWAP_TEL_S(emsplus_t01a5, mode_remain_time, tel->h.offs, len);
          SWAP_TEL_S(emsplus_t01a5, prg_mode_remain_time, tel->h.offs, len);
          SWAP_TEL_S(emsplus_t01a5, prg_mode_passed_time, tel->h.offs, len);
        break;
        default: break;
      }
    }
    break;
    case ETT_UBA_MON_FAST:
    {
      memcpy(((uint8_t *) &uba_mon_fast) + tel->h.offs, tel->d.raw, len);
      SWAP_TEL_S(uba_mon_fast, err, tel->h.offs, len);
      SWAP_TEL_S(uba_mon_fast, fl_current, tel->h.offs, len);
      SWAP_TEL_S(uba_mon_fast, tmp.rl, tel->h.offs, len);
      SWAP_TEL_S(uba_mon_fast, tmp.tmp1, tel->h.offs, len);
      SWAP_TEL_S(uba_mon_fast, tmp.water, tel->h.offs, len);
      SWAP_TEL_S(uba_mon_fast, vl_ist, tel->h.offs, len);
    }
    break;
    case ETT_UBA_MON_SLOW:
    {
      memcpy(((uint8_t *) &uba_mon_slow) + tel->h.offs, tel->d.raw, len);
      SWAP_TEL_S(uba_mon_slow, tmp_boiler, tel->h.offs, len);
      SWAP_TEL_S(uba_mon_slow, tmp_exhaust, tel->h.offs, len);
      SWAP_TEL_S(uba_mon_slow, tmp_out, tel->h.offs, len);
      uba_mon_slow.burner_starts_sane = HTONU_TRIVAL(uba_mon_slow.burner_starts);
      uba_mon_slow.run_time_heating_sane = HTON_TRIVAL(uba_mon_slow.run_time_heating);
      uba_mon_slow.run_time_sane = HTON_TRIVAL(uba_mon_slow.run_time);
      uba_mon_slow.run_time_stage_2_sane = HTON_TRIVAL(uba_mon_slow.run_time_stage_2);
    }
    break;
    case ETT_UBA_MON_WWM:
      memcpy(((uint8_t *) &uba_mon_wwm) + tel->h.offs, tel->d.raw, len);
      SWAP_TEL_S(uba_mon_wwm, ist[0], tel->h.offs, len);
      SWAP_TEL_S(uba_mon_wwm, ist[1], tel->h.offs, len);
      uba_mon_wwm.op_time_sane = HTONU_TRIVAL(uba_mon_wwm.op_time);
      uba_mon_wwm.op_count_sane = HTONU_TRIVAL(uba_mon_wwm.op_count);

    break;
    default: break;
  }
}


void ems_log_telegram(struct ems_telegram * tel, size_t len)
{
  float nan = 0.0 / 0.0;

  switch (tel->h.type)
  {
    case ETT_EMSPLUS:
    {
      switch(tel->d.emsplus.type)
      {
        case EMSPLUS_01A5:
          print_telegram(1, LL_DEBUG_MORE, "EMS+ Telegram 01a5", (uint8_t *) tel, len);
          LG_DEBUG("CW400 - Room Temp  %04.1f°C.", NANVAL(emsplus_t01a5.room_temp_act));
        break;
        default:
          print_telegram(0, LL_INFO, "Unknown EMS+ Telegram", (uint8_t *) tel, len);
        break;
      }
    }
    break;
    case ETT_UBA_MON_FAST:
    {
      struct ems_uba_monitor_fast * msg = &uba_mon_fast;
      print_telegram(1, LL_DEBUG_MORE, "EMS Telegram UBA_mon_fast", (uint8_t *) tel, len);

      LG_DEBUG("Mon fast   VL: %04.1f°C/%04.1f°C (ist/soll)  RL: %04.1f °C     WW: %04.1f °C     Tmp?: %04.1f °C."
          , 0.1f * msg->vl_ist, 1.0f * msg->vl_soll, NANVAL(msg->tmp.rl), NANVAL(msg->tmp.water), NANVAL(msg->tmp.tmp1 ));
      LG_DEBUG("  len: % 2u  Pump: %s       Blow: %s       Gas: %s      Ignite: %s      Circ: %s       Valve: %s.", len
                ,ONOFF(msg->on.pump), ONOFF(msg->on.blower), ONOFF(msg->on.gas    ), ONOFF(msg->on.ignite ), ONOFF(msg->on.circ   ), ONOFF(msg->on.valve  ));
      LG_DEBUG("  src: %02X  KsP: %03d%%/%03d%% (akt/max)    FlCurr: %04.1f µA Druck: %04.1f bar.", tel->h.src
                , msg->ks_akt_p, msg->ks_max_p, NANVAL(msg->fl_current), NANVA8(msg->sys_press));

      LG_DEBUG("  dst: %02X  Error: %d  ServiceCode: %.2s.", tel->h.dst, msg->err, msg->service_code);
    }
    break;
    case ETT_UBA_MON_SLOW:
    {
      struct ems_uba_monitor_slow * msg = &uba_mon_slow;

      print_telegram(1, LL_DEBUG_MORE, "EMS Telegram UBA_Mon_slow", (uint8_t *) tel, len);
      LG_DEBUG("Mon slow  Out: %04.1f °C    boiler: %04.1f °C    exhaust: %04.1f °C     Pump Mod: % 3u %%"
          , NANVAL(msg->tmp_out), NANVAL(msg->tmp_boiler), NANVAL(msg->tmp_exhaust), msg->pump_mod);
      LG_DEBUG("  len: % 2u       Burner starts: % 8d       runtime: % 8d min    rt-stage2: % 8d min      rt-heating: % 8d min.", len
                ,TRIVAL(msg->burner_starts), TRIVAL(msg->run_time), TRIVAL(msg->run_time_stage_2), TRIVAL(msg->run_time_heating));
      LG_DEBUG("  src: %02X     dst: %02X.", tel->h.src, tel->h.dst);
    }
    break;
    case ETT_UBA_MON_WWM:
    {
      struct ems_uba_monitor_wwm * msg = &uba_mon_wwm;

      print_telegram(1, LL_DEBUG_MORE, "EMS Telegram UBA_Mon_wwm", (uint8_t *) tel, len);
      LG_DEBUG("Mon WWM   Ist: %04.1f°C/%04.1f°C  Soll: %04.1f °C     Lädt: %s      Durchfluss: %04.1f l/m."
          , NANVAL(msg->ist[0]), NANVAL(msg->ist[1]), 1.0f * msg->soll, ONOFF(msg->sw2.is_loading), NANVA8(msg->throughput));
      LG_DEBUG("  len: % 2u Circ: %s       Man: %s       Day: %s      Single: %s      Day: %s       Reload: %s.", len
                ,ONOFF(msg->sw2.circ_active), ONOFF(msg->sw2.circ_manual), ONOFF(msg->sw2.circ_daylight), ONOFF(msg->sw1.single_load ), ONOFF(msg->sw1.daylight_mode), ONOFF(msg->sw1.reloading));
      LG_DEBUG("  src: %02X Type: %02X    prod: %s    Detox: %s     count: % 8u   time: % 8u min   temp: %s.", tel->h.src
                , msg->type, ONOFF(msg->sw1.active), ONOFF(msg->sw1.desinfect), TRIVAL(msg->op_count), TRIVAL(msg->op_time), msg->sw1.temp_ok ? " OK" : "NOK");
      LG_DEBUG("  dst: %02X WWErr: %s    PR1Err: %s   PR2Err: %s   DesErr: %s.", tel->h.dst
               , ONOFF(msg->fail.ww), ONOFF(msg->fail.probe_1), ONOFF(msg->fail.probe_2), ONOFF(msg->fail.desinfect));
    }
    break;
    default:
      print_telegram(0, LL_INFO, "Unknown EMS Telegram", (uint8_t *) tel, len);
    break;
  }

}

void ems_publish_telegram(struct mqtt_handle * mqtt, struct ems_telegram * tel, size_t len)
{
  len -= 5;
  switch (tel->h.type)
  {
    case ETT_EMSPLUS:
    {
      switch (tel->d.emsplus.type)
      {
        case EMSPLUS_01A5:
        {
          if (offsetof(typeof(emsplus_t01a5),room_temp_act) >= tel->h.offs && offsetof(typeof(emsplus_t01a5),room_temp_act) + sizeof((emsplus_t01a5).room_temp_act) - 1 <= ((tel->h.offs) + (len)))
               mqtt_publish(mqtt, "sensor", "CW400_room_temp", (emsplus_t01a5).room_temp_act);
        } while (0);
        break;
        default: break;
      }
    }
    break;
    case ETT_UBA_MON_FAST:
      CHECK_PUB(uba_mon_fast, tmp.water,"sensor", "uba_water", tel->h.offs, len);
      CHECK_PUB(uba_mon_fast, vl_ist, "sensor", "uba_vl_act", tel->h.offs, len);
      CHECK_PUB(uba_mon_fast, vl_soll, "sensor", "uba_vl_nom", tel->h.offs, len);
      CHECK_PUB_FLG(uba_mon_fast, on, pump, "relay","uba_on_pump", tel->h.offs, len);
      CHECK_PUB_FLG(uba_mon_fast, on, gas, "relay","uba_on_gas", tel->h.offs, len);
      CHECK_PUB_FLG(uba_mon_fast, on, valve, "relay","uba_on_valve", tel->h.offs, len);
      CHECK_PUB_FLG(uba_mon_fast, on, blower, "relay","uba_on_blower", tel->h.offs, len);
      CHECK_PUB_FLG(uba_mon_fast, on, circ, "relay","uba_on_circ", tel->h.offs, len);
      CHECK_PUB_FLG(uba_mon_fast, on, ignite, "relay","uba_on_igniter", tel->h.offs, len);
      CHECK_PUB(uba_mon_fast, ks_akt_p, "sensor","uba_power_act", tel->h.offs, len);
      CHECK_PUB(uba_mon_fast, ks_max_p, "sensor","uba_power_max", tel->h.offs, len);
      CHECK_PUB(uba_mon_fast, fl_current, "sensor", "uba_flame_current", tel->h.offs, len);
      CHECK_PUB(uba_mon_fast, err,        "sensor", "uba_error", tel->h.offs, len);
      CHECK_PUB_FORMATTED(uba_mon_fast, service_code, "sensor", "uba_service_code", "%.2s", tel->h.offs, len);
    break;
    case ETT_UBA_MON_SLOW:
      CHECK_PUB(uba_mon_slow, tmp_out      , "sensor", "uba_outside" , tel->h.offs, len);
      CHECK_PUB(uba_mon_slow, run_time_sane, "sensor", "uba_rt"      , tel->h.offs, len);
      CHECK_PUB(uba_mon_slow, pump_mod     , "sensor", "uba_pump_mod", tel->h.offs, len);
//      CHECK_PUB(uba_mon_slow, run_time_heating, "sensor", "uba_rt_heating", tel->h.offs, len);
//      CHECK_PUB(uba_mon_slow, run_time_stage_2, "sensor", "uba_rt_stage2", tel->h.offs, len);
//      CHECK_PUB(uba_mon_slow, burner_starts, "sensor", "uba_burner_starts", tel->h.offs, len);
    break;
    case ETT_UBA_MON_WWM:
      CHECK_PUB(uba_mon_wwm, ist[0],"sensor", "uba_ww_act1", tel->h.offs, len);
      CHECK_PUB(uba_mon_wwm, soll,"sensor", "uba_ww_nom", tel->h.offs, len);
      CHECK_PUB_FLG(uba_mon_wwm, sw2, circ_active, "relay", "uba_ww_circ_active", tel->h.offs, len);
      CHECK_PUB_FLG(uba_mon_wwm, sw2, circ_daylight, "relay", "uba_ww_circ_daylight", tel->h.offs, len);
      CHECK_PUB_FLG(uba_mon_wwm, sw2, circ_manual, "relay", "uba_ww_circ_manual", tel->h.offs, len);
      CHECK_PUB_FLG(uba_mon_wwm, sw2, is_loading, "relay", "uba_ww_loading", tel->h.offs, len);
      CHECK_PUB_FLG(uba_mon_wwm, sw1, active, "relay", "uba_ww_active", tel->h.offs, len);
      CHECK_PUB_FLG(uba_mon_wwm, sw1, single_load, "relay", "uba_ww_single_load", tel->h.offs, len);
      CHECK_PUB_FLG(uba_mon_wwm, sw1, reloading, "relay", "uba_ww_reloading", tel->h.offs, len);
      CHECK_PUB_FLG(uba_mon_wwm, sw1, daylight_mode, "relay", "uba_ww_daylight", tel->h.offs, len);
      CHECK_PUB_FLG(uba_mon_wwm, sw1, desinfect, "relay", "uba_ww_desinfect", tel->h.offs, len);
      CHECK_PUB_FLG(uba_mon_wwm, fail, desinfect, "relay", "uba_ww_fail_desinfect", tel->h.offs, len);
      CHECK_PUB_FLG(uba_mon_wwm, fail, probe_1, "relay", "uba_ww_fail_probe1", tel->h.offs, len);
      CHECK_PUB_FLG(uba_mon_wwm, fail, probe_2, "relay", "uba_ww_fail_probe2", tel->h.offs, len);
      CHECK_PUB_FLG(uba_mon_wwm, fail, ww, "relay", "uba_ww_fail", tel->h.offs, len);
    break;
    default: break;
  }
}


uint8_t msg_circ_on [] = { 0x8B, 0x08, 0x35, 0x00, 0x11, 0x11, 0x00 };
uint8_t msg_circ_off[] = { 0x8B, 0x08, 0x35, 0x00, 0x11, 0x01, 0x00 };

/* switch thermostat not boiler? */
uint8_t msg_circ2_off[] = { 0x8B, 0x10, 0xFF, 0x03, 0xF5, 0x02, 0x00, 0x00 };
uint8_t msg_circ2_on [] = { 0x8B, 0x10, 0xFF, 0x03, 0xF5, 0x02, 0x01, 0x00 };


void ems_logic_evaluate_telegram(struct ems_telegram * tel, size_t len)
{
  static int we_switched = FALSE;
  len -= 5;
  switch (tel->h.type)
  {
    case ETT_EMSPLUS:
    break;
    case ETT_UBA_MON_FAST:
      if (offsetof(struct ems_uba_monitor_fast, tmp.water) >= tel->h.offs && offsetof(struct ems_uba_monitor_fast, tmp.water) + sizeof(uba_mon_fast.tmp.water) - 1 <= tel->h.offs + len)
      {
        LG_INFO("Check if Water's too hot or too cold.");
        if (!uba_mon_wwm.sw2.circ_active)
        {
          we_switched = FALSE;  // reset if circulation is off
          if (uba_mon_fast.tmp.water >= 570)
          {
            LG_INFO("Water temp %f°C. Activate Circulation pump.", 0.1 * uba_mon_fast.tmp.water);
            mq_push(msg_circ2_on, sizeof(msg_circ2_on), FALSE);  // send message next time we are elected for bus master.
            we_switched = TRUE;
          }
        }
        else if (uba_mon_fast.tmp.water <= 560 && we_switched == TRUE)
        {
          LG_INFO("Water temp %f°C. Deactivate Circulation pump.", 0.1 * uba_mon_fast.tmp.water);
          mq_push(msg_circ2_off, sizeof(msg_circ2_off), FALSE);  // send message next time we are elected for bus master.
          we_switched = FALSE;
        }
      }
    break;
    case ETT_UBA_MON_SLOW:
    break;
    case ETT_UBA_MON_WWM:
    break;
    default: break;
  }
}

void print_telegram(int out, enum log_level loglevel, const char * prefix, uint8_t *msg, size_t len) {
    if (!log_get_level_state(loglevel))
        return;
    char text[3 + len * 3 + 2 + 1 + strlen(prefix) + 8];
    int pos = 0;
    pos += sprintf(&text[0], "%s (%02d) %cX:", prefix, len, out ? 'T' : 'R');

    for (size_t i = 0; i < len; i++) {
        pos += sprintf(&text[pos], " %02x", msg[i]);
        if (i == 3 || i == len - 2) {
            pos += sprintf(&text[pos], " ");
        }
    }
    log_push(loglevel, "%s", text);
}
