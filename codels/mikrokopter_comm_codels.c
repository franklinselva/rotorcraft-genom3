/*
 * Copyright (c) 2015-2017 LAAS/CNRS
 * All rights reserved.
 *
 * Redistribution and use  in source  and binary  forms,  with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *   1. Redistributions of  source  code must retain the  above copyright
 *      notice and this list of conditions.
 *   2. Redistributions in binary form must reproduce the above copyright
 *      notice and  this list of  conditions in the  documentation and/or
 *      other materials provided with the distribution.
 *
 *					Anthony Mallet on Fri Feb 13 2015
 */
#include "acmikrokopter.h"

#include <sys/time.h>

#include <assert.h>
#include <err.h>
#include <errno.h>
#include <float.h>
#include <fnmatch.h>
#include <math.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "mikrokopter_c_types.h"
#include "codels.h"


static or_time_ts	mk_get_ts(uint8_t seq, struct timeval rtv, double rate,
                                  mikrokopter_ids_sensor_time_s_ts_s *timings);


/* --- Task comm -------------------------------------------------------- */

/** Codel mk_comm_start of task comm.
 *
 * Triggered by mikrokopter_start.
 * Yields to mikrokopter_poll.
 * Throws mikrokopter_e_sys.
 */
genom_event
mk_comm_start(mikrokopter_conn_s **conn, const genom_context self)
{
  size_t i;

  *conn = malloc(sizeof(**conn));
  if (!*conn) return mk_e_sys_error(NULL, self);

  for(i = 0; i < mk_channels(); i++) {
    (*conn)->chan[i].fd = -1;
    (*conn)->chan[i].r = (*conn)->chan[i].w = 0;
  }

  return mikrokopter_poll;
}


/** Codel mk_comm_poll of task comm.
 *
 * Triggered by mikrokopter_poll.
 * Yields to mikrokopter_nodata, mikrokopter_recv.
 * Throws mikrokopter_e_sys.
 */
genom_event
mk_comm_poll(const mikrokopter_conn_s *conn, const genom_context self)
{
  int s;

  s = mk_wait_msg(conn->chan, mk_channels());
  if (s < 0) {
    if (errno != EINTR) return mk_e_sys_error(NULL, self);
    return mikrokopter_nodata;
  }
  else if (s == 0) return mikrokopter_nodata;

  return mikrokopter_recv;
}


/** Codel mk_comm_nodata of task comm.
 *
 * Triggered by mikrokopter_nodata.
 * Yields to mikrokopter_poll.
 * Throws mikrokopter_e_sys.
 */
genom_event
mk_comm_nodata(mikrokopter_conn_s **conn,
               mikrokopter_ids_sensor_time_s *sensor_time,
               const mikrokopter_imu *imu,
               mikrokopter_ids_battery_s *battery,
               const genom_context self)
{
  or_pose_estimator_state *idata = imu->data(self);

  /* reset exported data in case of timeout */
  idata->vel._present = false;
  idata->acc._present = false;
  battery->level = 0.;

  if (mk_set_sensor_rate(&sensor_time->rate, *conn, sensor_time, self))
    mk_disconnect_start(conn, self);

  return mikrokopter_poll;
}


/** Codel mk_comm_recv of task comm.
 *
 * Triggered by mikrokopter_recv.
 * Yields to mikrokopter_poll, mikrokopter_recv.
 * Throws mikrokopter_e_sys.
 */
genom_event
mk_comm_recv(mikrokopter_conn_s **conn,
             const mikrokopter_ids_imu_calibration_s *imu_calibration,
             mikrokopter_ids_sensor_time_s *sensor_time,
             const mikrokopter_imu *imu,
             mikrokopter_ids_rotor_data_s *rotor_data,
             mikrokopter_ids_battery_s *battery,
             const genom_context self)
{
  struct timeval tv;
  int more;
  size_t i;
  uint8_t *msg, len;
  int16_t v16;
  uint16_t u16;

  more = 0;
  for(i = 0; i < mk_channels(); i++) {
    if (mk_recv_msg(&(*conn)->chan[i], false) == 1) {
      more = 1;
      gettimeofday(&tv, NULL);

      msg = (*conn)->chan[i].msg;
      len = (*conn)->chan[i].len;
      switch(*msg++) {
        case 'I': /* IMU data */
          if (len == 14) {
            or_pose_estimator_state *idata = imu->data(self);
            double v[3];
            uint8_t seq = *msg++;

            idata->ts = mk_get_ts(
              seq, tv, sensor_time->rate.imu, &sensor_time->imu);
            idata->ts.sec = tv.tv_sec;
            idata->ts.nsec = 1000*tv.tv_usec;

            v16 = ((int16_t)(*msg++) << 8);
            v16 |= ((uint16_t)(*msg++) << 0);
            v[0] = v16/1000. + imu_calibration->abias[0];

            v16 = ((int16_t)(*msg++) << 8);
            v16 |= ((uint16_t)(*msg++) << 0);
            v[1] = v16/1000. + imu_calibration->abias[1];

            v16 = ((int16_t)(*msg++) << 8);
            v16 |= ((uint16_t)(*msg++) << 0);
            v[2] = v16/1000. + imu_calibration->abias[2];

            idata->acc._value.ax =
              imu_calibration->ascale[0] * v[0] +
              imu_calibration->ascale[1] * v[1] +
              imu_calibration->ascale[2] * v[2];
            idata->acc._value.ay =
              imu_calibration->ascale[3] * v[0] +
              imu_calibration->ascale[4] * v[1] +
              imu_calibration->ascale[5] * v[2];
            idata->acc._value.az =
              imu_calibration->ascale[6] * v[0] +
              imu_calibration->ascale[7] * v[1] +
              imu_calibration->ascale[8] * v[2];

            v16 = ((int16_t)(*msg++) << 8);
            v16 |= ((uint16_t)(*msg++) << 0);
            v[0] = v16/1000. + imu_calibration->gbias[0];

            v16 = ((int16_t)(*msg++) << 8);
            v16 |= ((uint16_t)(*msg++) << 0);
            v[1] = v16/1000. + imu_calibration->gbias[1];

            v16 = ((int16_t)(*msg++) << 8);
            v16 |= ((uint16_t)(*msg++) << 0);
            v[2] = v16/1000. + imu_calibration->gbias[2];

            idata->vel._value.vx = nan("");
            idata->vel._value.vy = nan("");
            idata->vel._value.vz = nan("");
            idata->vel._value.wx =
              imu_calibration->gscale[0] * v[0] +
              imu_calibration->gscale[1] * v[1] +
              imu_calibration->gscale[2] * v[2];
            idata->vel._value.wy =
              imu_calibration->gscale[3] * v[0] +
              imu_calibration->gscale[4] * v[1] +
              imu_calibration->gscale[5] * v[2];
            idata->vel._value.wz =
              imu_calibration->gscale[6] * v[0] +
              imu_calibration->gscale[7] * v[1] +
              imu_calibration->gscale[8] * v[2];

            idata->vel._present = true;
            idata->acc._present = true;
          }
          break;

        case 'M': /* motor data */
          if (len == 9) {
            uint8_t seq = *msg++;
            uint8_t state = *msg++;
            uint8_t id = state & 0xf;

            if (id < 1 || id > or_rotorcraft_max_rotors) break;
            id--;

            if (!rotor_data->state[id].ts.sec && rotor_data->state[id].disabled)
              rotor_data->state[id].disabled = 0;
            rotor_data->state[id].ts = mk_get_ts(
              seq, tv, sensor_time->rate.motor, &sensor_time->motor[id]);

            rotor_data->state[id].emerg = !!(state & 0x80);
            rotor_data->state[id].spinning = !!(state & 0x20);
            rotor_data->state[id].starting = !!(state & 0x10);

            v16 = ((int16_t)(*msg++) << 8);
            v16 |= ((uint16_t)(*msg++) << 0);
            if (rotor_data->state[id].spinning)
              rotor_data->state[id].velocity = v16 ? 1e6/2/v16 : 0.;
            else
              rotor_data->state[id].velocity = 0.;

            v16 = ((int16_t)(*msg++) << 8);
            v16 |= ((uint16_t)(*msg++) << 0);
            rotor_data->state[id].throttle = v16 * 100./1023.;

            u16 = ((uint16_t)(*msg++) << 8);
            u16 |= ((uint16_t)(*msg++) << 0);
            rotor_data->state[id].consumption = u16 / 1e3;
          }
          break;

        case 'B': /* battery data */
          if (len == 4) {
            uint8_t seq  __attribute__((unused)) = *msg++;
            double p;

            u16 = ((uint16_t)(*msg++) << 8);
            u16 |= ((uint16_t)(*msg++) << 0);
            battery->level = u16/1000.;

            p = 100. *
              (battery->level - battery->min)/(battery->max - battery->min);
            for(i = 0; i < or_rotorcraft_max_rotors; i++)
              rotor_data->state[i].energy_level = p;
          }
          break;

        case 'T': /* clock rate */
          if (len == 3) {
            uint8_t id = *msg++;

            if (id < 1 || id > or_rotorcraft_max_rotors) break;
            id--;
            rotor_data->clkrate[id] = *msg;
          }
          break;
      }
    }
  }

  return more ? mikrokopter_recv : mikrokopter_poll;
}


/** Codel mk_comm_stop of task comm.
 *
 * Triggered by mikrokopter_stop.
 * Yields to mikrokopter_ether.
 * Throws mikrokopter_e_sys.
 */
genom_event
mk_comm_stop(mikrokopter_conn_s **conn, const genom_context self)
{
  if (!*conn) return mikrokopter_ether;

  mk_send_msg(&(*conn)->chan[0], "x");
  mk_set_sensor_rate(
    &(struct mikrokopter_ids_sensor_time_s_rate_s){
      .imu = 0, .motor = 0, .battery = 0
        }, *conn, NULL, self);

  return mikrokopter_ether;
}


/* --- Activity connect ------------------------------------------------- */

/** Codel mk_connect_start of activity connect.
 *
 * Triggered by mikrokopter_start.
 * Yields to mikrokopter_ether.
 * Throws mikrokopter_e_sys, mikrokopter_e_baddev.
 */
genom_event
mk_connect_start(const char serial[2][64], uint32_t baud,
                 mikrokopter_conn_s **conn,
                 mikrokopter_ids_sensor_time_s *sensor_time,
                 const genom_context self)
{
  static const char magic[] = "[?]*[ml][ky][bfm][lu]1.8*";

  size_t i;
  int c, s;

  for(i = 0; i < mk_channels(); i++) {
    if ((*conn)->chan[i].fd >= 0) {
      close((*conn)->chan[i].fd);
      warnx("disconnected from %s", (*conn)->chan[i].path);
    }

    if (!*serial[i]) {
      (*conn)->chan[i].fd = -1;
      continue;
    }

    /* open tty */
    (*conn)->chan[i].fd = mk_open_tty(serial[i], baud);
    if ((*conn)->chan[i].fd < 0) return mk_e_sys_error(serial[i], self);

    (*conn)->chan[i].r = (*conn)->chan[i].w = 0;
    (*conn)->chan[i].start = (*conn)->chan[i].escape = false;

    /* check endpoint */
    while (mk_recv_msg(&(*conn)->chan[i], true) == 1); /* flush buffer */
    do {
      c = 0;
      do {
        if (mk_send_msg(&(*conn)->chan[i], "?")) /* ask for id */
          return mk_e_sys_error(serial[i], self);

        s = mk_wait_msg(&(*conn)->chan[i], 1);
        if (s < 0 && errno != EINTR) return mk_e_sys_error(NULL, self);
        if (s > 0) break;
      } while(c++ < 3);
      if (c > 3) {
        errno = ETIMEDOUT;
        return mk_e_sys_error(NULL, self);
      }

      s = mk_recv_msg(&(*conn)->chan[i], true);
    } while(s == 1 && (*conn)->chan[i].msg[0] != '?');

    (*conn)->chan[i].msg[(*conn)->chan[i].len] = 0;
    if (s != 1 || fnmatch(magic, (char *)(*conn)->chan[i].msg, 0)) {
      if (s != 1) {
        errno = ENOMSG;
        return mk_e_sys_error(NULL, self);
      } else {
        mikrokopter_e_baddev_detail d;
        snprintf(d.dev, sizeof(d.dev), "device is `%s' instead of `%s'",
                 (*conn)->chan[i].msg, magic);
        close((*conn)->chan[i].fd);
        (*conn)->chan[i].fd = -1;
        return mikrokopter_e_baddev(&d, self);
      }
    }

    snprintf((*conn)->chan[i].path, sizeof((*conn)->chan[i].path),
             "%s", serial[i]);
    warnx("connected to %s", (*conn)->chan[i].path);
  }

  /* configure data streaming */
  mk_set_sensor_rate(&sensor_time->rate, *conn, sensor_time, self);

  return mikrokopter_ether;
}


/* --- Activity disconnect ---------------------------------------------- */

/** Codel mk_disconnect_start of activity disconnect.
 *
 * Triggered by mikrokopter_start.
 * Yields to mikrokopter_ether.
 * Throws mikrokopter_e_sys.
 */
genom_event
mk_disconnect_start(mikrokopter_conn_s **conn,
                    const genom_context self)
{
  size_t i;

  mk_send_msg(&(*conn)->chan[0], "x");
  mk_set_sensor_rate(
    &(struct mikrokopter_ids_sensor_time_s_rate_s){
      .imu = 0, .motor = 0, .battery = 0
        }, *conn, NULL, self);

  for(i = 0; i < mk_channels(); i++) {
    if ((*conn)->chan[i].fd >= 0) {
      close((*conn)->chan[i].fd);
      warnx("disconnected from %s", (*conn)->chan[i].path);
    }
    (*conn)->chan[i].fd = -1;
  }

  return mikrokopter_ether;
}


/* --- Activity monitor ------------------------------------------------- */

/** Codel mk_monitor_check of activity monitor.
 *
 * Triggered by mikrokopter_start, mikrokopter_sleep.
 * Yields to mikrokopter_pause_sleep, mikrokopter_ether.
 * Throws mikrokopter_e_sys.
 */
genom_event
mk_monitor_check(const mikrokopter_conn_s *conn,
                 const genom_context self)
{
  size_t i;

  (void)self;

  for(i = 0; i < mk_channels(); i++)
    if (conn->chan[i].fd >= 0) return mikrokopter_pause_sleep;

  return mikrokopter_ether;
}


/* --- mk_get_ts ----------------------------------------------------------- */

/** Implements Olson, Edwin. "A passive solution to the sensor synchronization
 * problem." International conference on Intelligent Robots and Systems (IROS),
 * 2010 IEEE/RSJ */
static or_time_ts
mk_get_ts(uint8_t seq, struct timeval rtv, double rate,
          mikrokopter_ids_sensor_time_s_ts_s *timings)
{
  or_time_ts atv;
  double ts;
  uint8_t ds;
  assert(rate > 0.);

  /* delta samples */
  ds = seq - timings->seq;
  if (ds > 16)
    /* if too many samples were lost, we might have missed more than 255
     * samples: reset the offset */
    timings->offset = -DBL_MAX;
  else
    /* consider a 0.5% clock drift on the sender side */
    timings->offset -= 0.005 * ds / rate;

  /* update remote timestamp */
  timings->ts += ds / rate;
  timings->seq = seq;

  /* update offset */
  ts = rtv.tv_sec + 1e-6 * rtv.tv_usec;
  if (timings->ts - ts > timings->offset)
    timings->offset = timings->ts - ts;

  /* local timestamp */
  ts = timings->ts - timings->offset;
  atv.sec = floor(ts);
  atv.nsec = (ts - atv.sec) * 1e9;
  return atv;
}
