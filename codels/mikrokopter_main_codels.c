/*
 * Copyright (c) 2015-2016 LAAS/CNRS
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
#include <err.h>
#include <float.h>
#include <math.h>

#include "mikrokopter_c_types.h"
#include "codels.h"


/* --- Task main -------------------------------------------------------- */


/** Codel mk_main_init of task main.
 *
 * Triggered by mikrokopter_start.
 * Yields to mikrokopter_main.
 */
genom_event
mk_main_init(mikrokopter_ids *ids, const mikrokopter_rotors *rotors,
             const mikrokopter_propeller_measure *propeller_measure,
             const mikrokopter_imu *imu, genom_context self)
{
  genom_event e;
  int i;

  ids->sensor_time.rate.imu = 1000;
  ids->sensor_time.rate.motor = 100;
  ids->sensor_time.rate.battery = 1;
  e = mk_set_sensor_rate(
    &ids->sensor_time.rate, NULL, &ids->sensor_time, self);
  if (e) return e;

  ids->battery.alarm = 13.7;
  ids->battery.level = 0.;

  ids->imu_calibration.gscale[0] = 1.;
  ids->imu_calibration.gscale[1] = 0.;
  ids->imu_calibration.gscale[2] = 0.;
  ids->imu_calibration.gscale[3] = 0.;
  ids->imu_calibration.gscale[4] = 1.;
  ids->imu_calibration.gscale[5] = 0.;
  ids->imu_calibration.gscale[6] = 0.;
  ids->imu_calibration.gscale[7] = 0.;
  ids->imu_calibration.gscale[8] = 1.;
  ids->imu_calibration.gbias[0] = 0.;
  ids->imu_calibration.gbias[1] = 0.;
  ids->imu_calibration.gbias[2] = 0.;
  ids->imu_calibration.gstddev[0] = 1e-2;
  ids->imu_calibration.gstddev[1] = 1e-2;
  ids->imu_calibration.gstddev[2] = 1e-2;

  ids->imu_calibration.ascale[0] = 1.;
  ids->imu_calibration.ascale[1] = 0.;
  ids->imu_calibration.ascale[2] = 0.;
  ids->imu_calibration.ascale[3] = 0.;
  ids->imu_calibration.ascale[4] = 1.;
  ids->imu_calibration.ascale[5] = 0.;
  ids->imu_calibration.ascale[6] = 0.;
  ids->imu_calibration.ascale[7] = 0.;
  ids->imu_calibration.ascale[8] = 1.;
  ids->imu_calibration.abias[0] = 0.;
  ids->imu_calibration.abias[1] = 0.;
  ids->imu_calibration.abias[2] = 0.;
  ids->imu_calibration.astddev[0] = 5e-2;
  ids->imu_calibration.astddev[1] = 5e-2;
  ids->imu_calibration.astddev[2] = 5e-2;

  ids->imu_calibration_updated = true;

  ids->rotors_state._length = 0;
  for(i = 0; i < ids->rotors_state._maximum; i++) {
    ids->rotors_state._buffer[i].emerg = false;
    ids->rotors_state._buffer[i].spinning = false;
    ids->rotors_state._buffer[i].starting = false;
    ids->rotors_state._buffer[i].disabled = false;
  }

  ids->servo.vmin = 16.;
  ids->servo.vmax = 100.;

  rotors->data(self)->_length = 0;
  for(i = 0; i < rotors->data(self)->_maximum; i++) {
    rotors->data(self)->_buffer[i].state.emerg = false;
    rotors->data(self)->_buffer[i].state.spinning = false;
    rotors->data(self)->_buffer[i].state.starting = false;
    rotors->data(self)->_buffer[i].state.disabled = false;
  }
  propeller_measure->data(self)->w._length = 0;
  for(i = 0; i < propeller_measure->data(self)->w._maximum; i++) {
    propeller_measure->data(self)->w._buffer[i] = 0.;
    propeller_measure->data(self)->w._buffer[i] = 0.;
    propeller_measure->data(self)->w._buffer[i] = 0.;
    propeller_measure->data(self)->w._buffer[i] = 0.;
  }

  or_pose_estimator_state *imu_data = imu->data(self);
  imu_data->ts = (or_time_ts){ 0, 0 };
  imu_data->intrinsic = true;
  imu_data->pos._present = false;
  imu_data->pos_cov._present = false;
  imu_data->vel._present = false;
  imu_data->vel_cov._present = false;
  imu_data->acc._present = false;
  imu_data->acc_cov._present = false;

  return mikrokopter_main;
}


/** Codel mk_main_perm of task main.
 *
 * Triggered by mikrokopter_main.
 * Yields to mikrokopter_pause_main.
 */
genom_event
mk_main_perm(const mikrokopter_conn_s *conn,
             const mikrokopter_ids_battery_s *battery,
             const mikrokopter_ids_imu_calibration_s *imu_calibration,
             bool *imu_calibration_updated,
             const mikrokopter_rotors *rotors,
             const mikrokopter_propeller_measure *propeller_measure,
             const mikrokopter_imu *imu, genom_context self)
{
  /* battery level */
  if (conn && conn->chan[0].fd >= 0) {
    if (battery->level > 0. && battery->level < battery->alarm) {
      static int cnt;

      if (!cnt) mk_send_msg(&conn->chan[0], "~%2", 440);
      cnt = (cnt + 1) % 500;
    }
  }

  /* imu covariance data */
  if (*imu_calibration_updated) {
    or_pose_estimator_state *idata = imu->data(self);

    idata->vel_cov._value.cov[0] = 0.;
    idata->vel_cov._value.cov[1] = 0.;
    idata->vel_cov._value.cov[2] = 0.;
    idata->vel_cov._value.cov[3] = 0.;
    idata->vel_cov._value.cov[4] = 0.;
    idata->vel_cov._value.cov[5] = 0.;
    idata->vel_cov._value.cov[6] = 0.;
    idata->vel_cov._value.cov[7] = 0.;
    idata->vel_cov._value.cov[8] = 0.;
    idata->vel_cov._value.cov[9] =
      imu_calibration->gstddev[0] * imu_calibration->gstddev[0];
    idata->vel_cov._value.cov[10] = 0.;
    idata->vel_cov._value.cov[11] = 0.;
    idata->vel_cov._value.cov[12] = 0.;
    idata->vel_cov._value.cov[13] = 0.;
    idata->vel_cov._value.cov[14] =
      imu_calibration->gstddev[1] * imu_calibration->gstddev[1];
    idata->vel_cov._value.cov[15] = 0.;
    idata->vel_cov._value.cov[16] = 0.;
    idata->vel_cov._value.cov[17] = 0.;
    idata->vel_cov._value.cov[18] = 0.;
    idata->vel_cov._value.cov[19] = 0.;
    idata->vel_cov._value.cov[20] =
      imu_calibration->gstddev[2] * imu_calibration->gstddev[2];
    idata->vel_cov._present = true;

    idata->acc_cov._value.cov[0] =
      imu_calibration->astddev[0] * imu_calibration->astddev[0];
    idata->acc_cov._value.cov[1] = 0.;
    idata->acc_cov._value.cov[2] =
      imu_calibration->astddev[1] * imu_calibration->astddev[1];
    idata->acc_cov._value.cov[3] = 0.;
    idata->acc_cov._value.cov[4] = 0.;
    idata->acc_cov._value.cov[5] =
      imu_calibration->astddev[2] * imu_calibration->astddev[2];
    idata->acc_cov._present = true;

    *imu_calibration_updated = false;
  }


  /* publish */
  rotors->write(self);
  propeller_measure->write(self);
  imu->write(self);

  return mikrokopter_pause_main;
}


/** Codel mk_main_stop of task main.
 *
 * Triggered by mikrokopter_stop.
 * Yields to mikrokopter_ether.
 */
genom_event
mk_main_stop(mikrokopter_log_s **log, genom_context self)
{
  mk_log_stop(log, self);
  return mikrokopter_ether;
}


/* --- Activity calibrate_imu ------------------------------------------- */

/** Codel mk_calibrate_imu_start of activity calibrate_imu.
 *
 * Triggered by mikrokopter_start.
 * Yields to mikrokopter_collect.
 * Throws mikrokopter_e_sys, mikrokopter_e_connection.
 */
genom_event
mk_calibrate_imu_start(double tstill, uint16_t nposes,
                       genom_context self)
{
  uint32_t sps;
  int s;

  sps = 1000/mikrokopter_control_period_ms;
  s = mk_calibration_init(tstill * sps, nposes, sps);
  if (s) {
    errno = s;
    return mk_e_sys_error("calibration", self);
  }

  warnx("calibration started");
  return mikrokopter_collect;
}

/** Codel mk_calibrate_imu_collect of activity calibrate_imu.
 *
 * Triggered by mikrokopter_collect.
 * Yields to mikrokopter_pause_collect, mikrokopter_main.
 * Throws mikrokopter_e_sys, mikrokopter_e_connection.
 */
genom_event
mk_calibrate_imu_collect(const mikrokopter_imu *imu,
                         genom_context self)
{
  int32_t still;
  int s;

  s = mk_calibration_collect(imu->data(self), &still);
  switch(s) {
    case 0: break;

    case EAGAIN:
      if (still == 0)
        warnx("stay still");
      else if (still > 0)
        warnx("calibration acquired pose %d", still);
      return mikrokopter_pause_collect;

    default:
      warnx("calibration aborted");
      mk_calibration_fini(NULL, NULL, NULL, NULL);
      errno = s;
      return mk_e_sys_error("calibration", self);
  }

  warnx("calibration acquired all poses");
  return mikrokopter_main;
}

/** Codel mk_calibrate_imu_main of activity calibrate_imu.
 *
 * Triggered by mikrokopter_main.
 * Yields to mikrokopter_ether.
 * Throws mikrokopter_e_sys, mikrokopter_e_connection.
 */
genom_event
mk_calibrate_imu_main(mikrokopter_ids_imu_calibration_s *imu_calibration,
                      bool *imu_calibration_updated,
                      genom_context self)
{
  double maxa[3], maxw[3];
  int s;

  s = mk_calibration_acc(imu_calibration->ascale, imu_calibration->abias);
  if (s) {
    mk_calibration_fini(NULL, NULL, NULL, NULL);
    errno = s;
    return mk_e_sys_error("calibration", self);
  }

  s = mk_calibration_gyr(imu_calibration->gscale, imu_calibration->gbias);
  if (s) {
    mk_calibration_fini(NULL, NULL, NULL, NULL);
    errno = s;
    return mk_e_sys_error("calibration", self);
  }

  mk_calibration_fini(imu_calibration->astddev, imu_calibration->gstddev,
                      maxa, maxw);
  warnx("calibration max acceleration: "
        "x %.2fm/s², y %.2fm/s², z %.2fm/s²", maxa[0], maxa[1], maxa[2]);
  warnx("calibration max angular velocity: "
        "x %.2f⁰/s, y %.2f⁰/s, z %.2f⁰/s",
        maxw[0] * 180./M_PI, maxw[1] * 180./M_PI, maxw[2] * 180./M_PI);

  *imu_calibration_updated = true;
  return mikrokopter_ether;
}


/* --- Activity set_zero ------------------------------------------------ */

/** Codel mk_set_zero_start of activity set_zero.
 *
 * Triggered by mikrokopter_start.
 * Yields to mikrokopter_collect.
 * Throws mikrokopter_e_sys.
 */
genom_event
mk_set_zero_start(double accum[3], double gycum[3], uint32_t *n,
                  genom_context self)
{
  gycum[0] = gycum[1] = gycum[2] = 0.;
  accum[0] = accum[1] = accum[2] = 0.;
  *n = 0;
  return mikrokopter_collect;
}

/** Codel mk_set_zero_collect of activity set_zero.
 *
 * Triggered by mikrokopter_collect.
 * Yields to mikrokopter_pause_collect, mikrokopter_main.
 * Throws mikrokopter_e_sys.
 */
genom_event
mk_set_zero_collect(const mikrokopter_imu *imu, double accum[3],
                    double gycum[3], uint32_t *n, genom_context self)
{
  or_pose_estimator_state *imu_data = imu->data(self);

  if (!imu_data->vel._present || !imu_data->acc._present) {
    errno = EIO;
    return mk_e_sys_error("set_zero", self);
  }

  gycum[0] = (*n * gycum[0] - imu_data->vel._value.wx) / (1 + *n);
  gycum[1] = (*n * gycum[1] - imu_data->vel._value.wy) / (1 + *n);
  gycum[2] = (*n * gycum[2] - imu_data->vel._value.wz) / (1 + *n);

  accum[0] = (*n * accum[0] + imu_data->acc._value.ax) / (1 + *n);
  accum[1] = (*n * accum[1] + imu_data->acc._value.ay) / (1 + *n);
  accum[2] = (*n * accum[2] + imu_data->acc._value.az) / (1 + *n);

  return ((*n)++ < 2000.) ? mikrokopter_pause_collect : mikrokopter_main;
}

/** Codel mk_set_zero of activity set_zero.
 *
 * Triggered by mikrokopter_main.
 * Yields to mikrokopter_ether.
 * Throws mikrokopter_e_sys.
 */
genom_event
mk_set_zero(double accum[3], double gycum[3],
            mikrokopter_ids_imu_calibration_s *imu_calibration,
            bool *imu_calibration_updated, genom_context self)
{
  double roll, pitch;
  double cr, cp, sr, sp;
  double r[9];

  roll = atan2(accum[1], accum[2]);
  cr = cos(roll);  sr = sin(roll);
  pitch = atan2(-accum[0] * cr, accum[2]);
  cp = cos(pitch); sp = sin(pitch);

  r[0] = cp;   r[1] = sr * sp;  r[2] = cr * sp;
  r[3] = 0.;   r[4] = cr;       r[5] = -sr;
  r[6] = -sp;  r[7] = cp * sr;  r[8] = cr * cp;

  mk_calibration_bias(gycum, imu_calibration->gscale, imu_calibration->gbias);
  mk_calibration_rotate(r, imu_calibration->gscale);
  mk_calibration_rotate(r, imu_calibration->ascale);

  *imu_calibration_updated = true;
  return mikrokopter_ether;
}


/* --- Activity start --------------------------------------------------- */

/** Codel mk_start_start of activity start.
 *
 * Triggered by mikrokopter_start.
 * Yields to mikrokopter_monitor.
 * Throws mikrokopter_e_connection, mikrokopter_e_started,
 *        mikrokopter_e_rotor_failure,
 *        mikrokopter_e_rotor_not_disabled.
 */
genom_event
mk_start_start(const mikrokopter_conn_s *conn, uint16_t *state,
               const sequence8_mikrokopter_rotor_state_s *rotors_state,
               genom_context self)
{
  int i;

  if (!conn) return mikrokopter_e_connection(self);
  for(i = 0; i < rotors_state->_length; i++) {
    if (i < rotors_state->_length && rotors_state->_buffer[i].disabled)
      continue;
    if (rotors_state->_buffer[i].spinning)
      return mikrokopter_e_started(self);
  }

  *state = 0;
  mk_send_msg(&conn->chan[0], "g");
  return mikrokopter_monitor;
}

/** Codel mk_start_monitor of activity start.
 *
 * Triggered by mikrokopter_monitor.
 * Yields to mikrokopter_pause_monitor, mikrokopter_ether.
 * Throws mikrokopter_e_connection, mikrokopter_e_started,
 *        mikrokopter_e_rotor_failure,
 *        mikrokopter_e_rotor_not_disabled.
 */
genom_event
mk_start_monitor(const mikrokopter_conn_s *conn, uint16_t *state,
                 const sequence8_mikrokopter_rotor_state_s *rotors_state,
                 genom_context self)
{
  mikrokopter_e_rotor_failure_detail e;
  mikrokopter_e_rotor_not_disabled_detail d;
  int i;

  for(i = 0; i < rotors_state->_length; i++) {
    if (rotors_state->_buffer[i].spinning) {
      if (rotors_state->_buffer[i].disabled) {
        mk_send_msg(&conn->chan[0], "x");
        d.id = 1 + i;
        return mikrokopter_e_rotor_not_disabled(&d, self);
      }

      continue;
    }
    if (rotors_state->_buffer[i].disabled) continue;

    if (rotors_state->_buffer[i].starting) *state |= 1 << i;

    if ((!rotors_state->_buffer[i].starting && (*state & (1 << i))) ||
        rotors_state->_buffer[i].emerg) {
      mk_send_msg(&conn->chan[0], "x");
      e.id = 1 + i;
      return mikrokopter_e_rotor_failure(&e, self);
    }

    return mikrokopter_pause_monitor;
  }

  return mikrokopter_ether;
}

/** Codel mk_start_stop of activity start.
 *
 * Triggered by mikrokopter_stop.
 * Yields to mikrokopter_ether.
 * Throws mikrokopter_e_connection, mikrokopter_e_started,
 *        mikrokopter_e_rotor_failure,
 *        mikrokopter_e_rotor_not_disabled.
 */
genom_event
mk_start_stop(const mikrokopter_conn_s *conn, genom_context self)
{
  mk_send_msg(&conn->chan[0], "x");
  return mikrokopter_ether;
}


/* --- Activity servo --------------------------------------------------- */

/** Codel mk_servo_main of activity servo.
 *
 * Triggered by mikrokopter_start.
 * Yields to mikrokopter_pause_start, mikrokopter_stop.
 * Throws mikrokopter_e_connection, mikrokopter_e_rotor_failure,
 *        mikrokopter_e_input.
 */
genom_event
mk_servo_main(const mikrokopter_conn_s *conn,
              const sequence8_mikrokopter_rotor_state_s *rotors_state,
              const mikrokopter_propeller_input *propeller_input,
              genom_context self)
{
  or_rotorcraft_input *input_data;
  struct timeval tv;
  genom_event e;

  if (!conn) return mikrokopter_e_connection(self);

  /* update input */
  if (propeller_input->read(self)) return mikrokopter_e_input(self);

  input_data = propeller_input->data(self);
  if (!input_data) return mikrokopter_e_input(self);

  /* watchdog */
  gettimeofday(&tv, NULL);
  if (tv.tv_sec + 1e-6*tv.tv_usec >
      0.5 + input_data->ts.sec + 1e-9*input_data->ts.nsec) {
    /* do something smart here, instead of the following */
    return mikrokopter_stop;
  }

  /* send */
  e = mk_set_velocity(conn, rotors_state, &input_data->w, self);
  if (e) return e;

  return mikrokopter_pause_start;
}

/** Codel mk_servo_stop of activity servo.
 *
 * Triggered by mikrokopter_stop.
 * Yields to mikrokopter_ether.
 * Throws mikrokopter_e_connection, mikrokopter_e_rotor_failure,
 *        mikrokopter_e_input.
 */
genom_event
mk_servo_stop(const mikrokopter_conn_s *conn, genom_context self)
{
  uint16_t p[or_rotorcraft_max_rotors];
  int i;

  for(i = 0; i < or_rotorcraft_max_rotors; i++) p[i] = 65535;

  mk_send_msg(&conn->chan[0], "w%@", p, or_rotorcraft_max_rotors);

  return mikrokopter_ether;
}
