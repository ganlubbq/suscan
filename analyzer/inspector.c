/*

  Copyright (C) 2017 Gonzalo José Carracedo Carballal

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation, either version 3 of the
  License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this program.  If not, see
  <http://www.gnu.org/licenses/>

*/

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdint.h>

#define SU_LOG_DOMAIN "suscan-inspector"

#include <sigutils/sigutils.h>
#include <sigutils/detect.h>
#include <sigutils/sampling.h>

#include "mq.h"
#include "msg.h"

#define SUSCAN_INSPECTOR_BETA .35

void
suscan_inspector_destroy(suscan_inspector_t *insp)
{
  if (insp->fac_baud_det != NULL)
    su_channel_detector_destroy(insp->fac_baud_det);

  if (insp->nln_baud_det != NULL)
    su_channel_detector_destroy(insp->nln_baud_det);

  su_agc_finalize(&insp->agc);

  su_costas_finalize(&insp->costas_2);

  su_costas_finalize(&insp->costas_4);

  free(insp);
}

/* Spike durations measured in symbol times */
#define SUSCAN_INSPECTOR_FAST_RISE_FRAC   3.9062e-1
#define SUSCAN_INSPECTOR_FAST_FALL_FRAC   (2 * SUSCAN_INSPECTOR_FAST_RISE_FRAC)
#define SUSCAN_INSPECTOR_SLOW_RISE_FRAC   (10 * SUSCAN_INSPECTOR_FAST_RISE_FRAC)
#define SUSCAN_INSPECTOR_SLOW_FALL_FRAC   (10 * SUSCAN_INSPECTOR_FAST_FALL_FRAC)
#define SUSCAN_INSPECTOR_HANG_MAX_FRAC    0.19531
#define SUSCAN_INSPECTOR_DELAY_LINE_FRAC  0.39072
#define SUSCAN_INSPECTOR_MAG_HISTORY_FRAC 0.39072

suscan_inspector_t *
suscan_inspector_new(
    const suscan_analyzer_t *analyzer,
    const struct sigutils_channel *channel)
{
  suscan_inspector_t *new;
  struct sigutils_channel_detector_params params =
      sigutils_channel_detector_params_INITIALIZER;
  struct su_agc_params agc_params = su_agc_params_INITIALIZER;
  SUFLOAT tau;

  if ((new = calloc(1, sizeof (suscan_inspector_t))) == NULL)
    goto fail;

  new->state = SUSCAN_ASYNC_STATE_CREATED;

  /* Common channel parameters */
  su_channel_params_adjust_to_channel(&params, channel);

  params.samp_rate = analyzer->source.detector->params.samp_rate;
  params.window_size = SUSCAN_SOURCE_DEFAULT_BUFSIZ;
  params.alpha = 1e-4;

  /* Create generic autocorrelation-based detector */
  params.mode = SU_CHANNEL_DETECTOR_MODE_AUTOCORRELATION;
  if ((new->fac_baud_det = su_channel_detector_new(&params)) == NULL)
    goto fail;

  /* Create non-linear baud rate detector */
  params.mode = SU_CHANNEL_DETECTOR_MODE_NONLINEAR_DIFF;
  if ((new->nln_baud_det = su_channel_detector_new(&params)) == NULL)
    goto fail;

  /* Initialize local oscillator */
  su_ncqo_init(&new->lo, 0);
  new->phase = 1.;

  /* Initialize AGC */
  tau = params.samp_rate / params.bw; /* Samples per symbol */

  agc_params.fast_rise_t = tau * SUSCAN_INSPECTOR_FAST_RISE_FRAC;
  agc_params.fast_fall_t = tau * SUSCAN_INSPECTOR_FAST_FALL_FRAC;
  agc_params.slow_rise_t = tau * SUSCAN_INSPECTOR_SLOW_RISE_FRAC;
  agc_params.slow_fall_t = tau * SUSCAN_INSPECTOR_SLOW_FALL_FRAC;
  agc_params.hang_max    = tau * SUSCAN_INSPECTOR_HANG_MAX_FRAC;

  agc_params.delay_line_size  = tau * SUSCAN_INSPECTOR_DELAY_LINE_FRAC;
  agc_params.mag_history_size = tau * SUSCAN_INSPECTOR_MAG_HISTORY_FRAC;

  SU_TRYCATCH(su_agc_init(&new->agc, &agc_params), goto fail);

  /* Initialize PLLs */
  SU_TRYCATCH(
      su_costas_init(
          &new->costas_2,
          SU_COSTAS_KIND_BPSK,
          0,
          SU_ABS2NORM_FREQ(params.samp_rate, params.bw),
          3,
          1e-2 * SU_ABS2NORM_FREQ(params.samp_rate, params.bw)),
      goto fail);

  SU_TRYCATCH(
      su_costas_init(
          &new->costas_4,
          SU_COSTAS_KIND_QPSK,
          0,
          SU_ABS2NORM_FREQ(params.samp_rate, params.bw),
          3,
          1e-2 * SU_ABS2NORM_FREQ(params.samp_rate, params.bw)),
      goto fail);

  return new;

fail:
  if (new != NULL)
    suscan_inspector_destroy(new);

  return NULL;
}

int
suscan_inspector_feed_bulk(
    suscan_inspector_t *insp,
    const SUCOMPLEX *x,
    int count)
{
  int i;
  SUFLOAT alpha;
  SUCOMPLEX det_x;
  SUCOMPLEX sample;
  SUCOMPLEX samp_phase_samples = insp->params.sym_phase * insp->sym_period;
  SUBOOL ok = SU_FALSE;

  insp->sym_new_sample = SU_FALSE;

  for (i = 0; i < count && !insp->sym_new_sample; ++i) {
    /*
     * Feed channel detectors. TODO: use last_window_sample with nln_baud_det
     */
    SU_TRYCATCH(
        su_channel_detector_feed(insp->fac_baud_det, x[i]),
        goto done);
    SU_TRYCATCH(
        su_channel_detector_feed(insp->nln_baud_det, x[i]),
        goto done);

    /*
     * Feed AGC. We use the last windowed sample from one of the
     * channel detectors
     */

    det_x = insp->fac_baud_det->last_window_sample;

    /* Perform carrier control */
    det_x *= SU_C_CONJ(su_ncqo_read(&insp->lo)) * insp->phase;
    det_x  = 2 * su_agc_feed(&insp->agc, det_x) * 1.4142;

    switch (insp->params.fc_ctrl) {
      case SUSCAN_INSPECTOR_CARRIER_CONTROL_MANUAL:
        sample = det_x;
        break;

      case SUSCAN_INSPECTOR_CARRIER_CONTROL_COSTAS_2:
        su_costas_feed(&insp->costas_2, det_x);
        sample = insp->costas_2.y;
        break;

      case SUSCAN_INSPECTOR_CARRIER_CONTROL_COSTAS_4:
        su_costas_feed(&insp->costas_4, det_x);
        sample = insp->costas_4.y;
        break;
    }

    /* Check if channel sampler is enabled */
    if (insp->sym_period >= 1.) {
      insp->sym_phase += 1.;
      if (insp->sym_phase >= insp->sym_period)
        insp->sym_phase -= insp->sym_period;

      insp->sym_new_sample =
          (int) SU_FLOOR(insp->sym_phase - samp_phase_samples) == 0;

      if (insp->sym_new_sample) {
        alpha = insp->sym_phase - SU_FLOOR(insp->sym_phase);

        insp->sym_sampler_output =
            .5 * ((1 - alpha) * insp->sym_last_sample + alpha * sample);

      }
    }

    insp->sym_last_sample = sample;
  }

  ok = SU_TRUE;

done:
  return ok ? i : -1;
}

/*
 * TODO: Store *one port* only per worker. This port is read once all
 * consumers have finished with their buffer.
 */
SUPRIVATE SUBOOL
suscan_inspector_wk_cb(
    struct suscan_mq *mq_out,
    void *wk_private,
    void *cb_private)
{
  suscan_consumer_t *consumer = (suscan_consumer_t *) wk_private;
  suscan_inspector_t *insp = (suscan_inspector_t *) cb_private;
  unsigned int i;
  int fed;
  SUSCOUNT got;
  SUCOMPLEX *samp;
  struct suscan_analyzer_sample_batch_msg *batch_msg = NULL;
  SUBOOL restart = SU_FALSE;

  if (insp->task_state.consumer == NULL)
    suscan_consumer_task_state_init(&insp->task_state, consumer);

  if (insp->state == SUSCAN_ASYNC_STATE_HALTING)
    goto done;

  if (!suscan_consumer_task_state_assert_samples(
      &insp->task_state,
      &samp,
      &got))
    goto done;

  while (got > 0) {
    SU_TRYCATCH(
        (fed = suscan_inspector_feed_bulk(insp, samp, got)) >= 0,
        goto done);

    if (insp->sym_new_sample) {
      /* Sampler was triggered */
      if (batch_msg == NULL)
        SU_TRYCATCH(
            batch_msg = suscan_analyzer_sample_batch_msg_new(
                insp->params.inspector_id),
            goto done);

      SU_TRYCATCH(
          suscan_analyzer_sample_batch_msg_append_sample(
              batch_msg,
              insp->sym_sampler_output),
          goto done);

    }

    /* Consume all these */
    suscan_consumer_task_state_advance(&insp->task_state, fed);

    samp += fed;
    got  -= fed;
  }

  /* Got samples, send message batch */
  if (batch_msg != NULL) {
    SU_TRYCATCH(
        suscan_mq_write(
            consumer->analyzer->mq_out,
            SUSCAN_ANALYZER_MESSAGE_TYPE_SAMPLES,
            batch_msg),
        goto done);
    batch_msg = NULL;
  }

  restart = SU_TRUE;

done:
  if (!restart) {
    insp->state = SUSCAN_ASYNC_STATE_HALTED;
    suscan_consumer_remove_task(insp->task_state.consumer);
  }

  if (batch_msg != NULL)
    suscan_analyzer_sample_batch_msg_destroy(batch_msg);

  return restart;
}

SUINLINE suscan_inspector_t *
suscan_analyzer_get_inspector(
    const suscan_analyzer_t *analyzer,
    SUHANDLE handle)
{
  suscan_inspector_t *brinsp;

  if (handle < 0 || handle >= analyzer->inspector_count)
    return NULL;

  brinsp = analyzer->inspector_list[handle];

  if (brinsp != NULL && brinsp->state != SUSCAN_ASYNC_STATE_RUNNING)
    return NULL;

  return brinsp;
}

SUPRIVATE SUBOOL
suscan_analyzer_dispose_inspector_handle(
    suscan_analyzer_t *analyzer,
    SUHANDLE handle)
{
  if (handle < 0 || handle >= analyzer->inspector_count)
    return SU_FALSE;

  if (analyzer->inspector_list[handle] == NULL)
    return SU_FALSE;

  analyzer->inspector_list[handle] = NULL;

  return SU_TRUE;
}

SUPRIVATE SUHANDLE
suscan_analyzer_register_inspector(
    suscan_analyzer_t *analyzer,
    suscan_inspector_t *brinsp)
{
  SUHANDLE hnd;

  if (brinsp->state != SUSCAN_ASYNC_STATE_CREATED)
    return SU_FALSE;

  /* Plugged. Append handle to list */
  /* TODO: Find inspectors in HALTED state, and free them */
  if ((hnd = PTR_LIST_APPEND_CHECK(analyzer->inspector, brinsp)) == -1)
    return -1;

  /* Mark it as running and push to worker */
  brinsp->state = SUSCAN_ASYNC_STATE_RUNNING;

  if (!suscan_analyzer_push_task(
      analyzer,
      suscan_inspector_wk_cb,
      brinsp)) {
    suscan_analyzer_dispose_inspector_handle(analyzer, hnd);
    return -1;
  }

  return hnd;
}

/*
 * We have ownership on msg, this messages are urgent: they are placed
 * in the beginning of the queue
 */
SUBOOL
suscan_analyzer_parse_inspector_msg(
    suscan_analyzer_t *analyzer,
    struct suscan_analyzer_inspector_msg *msg)
{
  suscan_inspector_t *new = NULL;
  suscan_inspector_t *insp = NULL;
  SUSCOUNT fs;
  SUHANDLE handle = -1;
  SUBOOL ok = SU_FALSE;

  switch (msg->kind) {
    case SUSCAN_ANALYZER_INSPECTOR_MSGKIND_OPEN:
      if ((new = suscan_inspector_new(
          analyzer,
          &msg->channel)) == NULL)
        goto done;

      handle = suscan_analyzer_register_inspector(analyzer, new);
      if (handle == -1)
        goto done;
      new = NULL;

      msg->handle = handle;
      break;

    case SUSCAN_ANALYZER_INSPECTOR_MSGKIND_GET_INFO:
      if ((insp = suscan_analyzer_get_inspector(
          analyzer,
          msg->handle)) == NULL) {
        /* No such handle */
        msg->kind = SUSCAN_ANALYZER_INSPECTOR_MSGKIND_WRONG_HANDLE;
      } else {
        /* Retrieve current esimate for message kind */
        msg->kind = SUSCAN_ANALYZER_INSPECTOR_MSGKIND_INFO;
        msg->baud.fac = insp->fac_baud_det->baud;
        msg->baud.nln = insp->nln_baud_det->baud;
      }
      break;

    case SUSCAN_ANALYZER_INSPECTOR_MSGKIND_GET_PARAMS:
      if ((insp = suscan_analyzer_get_inspector(
          analyzer,
          msg->handle)) == NULL) {
        /* No such handle */
        msg->kind = SUSCAN_ANALYZER_INSPECTOR_MSGKIND_WRONG_HANDLE;
      } else {
        /* Retrieve current inspector params */
        msg->kind = SUSCAN_ANALYZER_INSPECTOR_MSGKIND_PARAMS;
        msg->params = insp->params;
      }
      break;

    case SUSCAN_ANALYZER_INSPECTOR_MSGKIND_PARAMS:
      if ((insp = suscan_analyzer_get_inspector(
          analyzer,
          msg->handle)) == NULL) {
        /* No such handle */
        msg->kind = SUSCAN_ANALYZER_INSPECTOR_MSGKIND_WRONG_HANDLE;
      } else {
        insp->params = msg->params;
        fs = insp->fac_baud_det->params.samp_rate;

        /* Update inspector according to params */
        if (msg->params.baud > 0)
          insp->sym_period = 1. / SU_ABS2NORM_BAUD(fs, insp->params.baud);
        else
          insp->sym_period = 0;

        /* Update local oscillator frequency and phase */
        su_ncqo_set_freq(
            &insp->lo,
            SU_ABS2NORM_FREQ(fs, msg->params.fc_off));
        insp->phase = SU_C_EXP(I * msg->params.fc_phi);
      }
      break;

    case SUSCAN_ANALYZER_INSPECTOR_MSGKIND_CLOSE:
      if ((insp = suscan_analyzer_get_inspector(
          analyzer,
          msg->handle)) == NULL) {
        msg->kind = SUSCAN_ANALYZER_INSPECTOR_MSGKIND_WRONG_HANDLE;
      } else {
        msg->inspector_id = insp->params.inspector_id;

        if (insp->state == SUSCAN_ASYNC_STATE_HALTED) {
          /*
           * Inspector has been halted. It's safe to dispose the handle
           * and free the object.
           */
          (void) suscan_analyzer_dispose_inspector_handle(
              analyzer,
              msg->handle);
          suscan_inspector_destroy(insp);
        } else {
          /*
           * Inspector is still running. Mark it as halting, so it will not
           * come back to the worker queue.
           */
          insp->state = SUSCAN_ASYNC_STATE_HALTING;
        }

        /* We can't trust the inspector contents from here on out */
        insp = NULL;
      }
      break;

    default:
      msg->status = msg->kind;
      msg->kind = SUSCAN_ANALYZER_INSPECTOR_MSGKIND_WRONG_KIND;
  }

  /*
   * If request has referenced an existing inspector, we include the
   * inspector ID in the response.
   */
  if (insp != NULL)
    msg->inspector_id = insp->params.inspector_id;

  if (!suscan_mq_write(
      analyzer->mq_out,
      SUSCAN_ANALYZER_MESSAGE_TYPE_INSPECTOR,
      msg))
    goto done;

  ok = SU_TRUE;

done:
  if (new != NULL)
    suscan_inspector_destroy(new);

  return ok;
}

/************************* Channel inspector API ****************************/
SUBOOL
suscan_inspector_open_async(
    suscan_analyzer_t *analyzer,
    const struct sigutils_channel *channel,
    uint32_t req_id)
{
  struct suscan_analyzer_inspector_msg *req = NULL;
  uint32_t type;
  SUBOOL ok = SU_FALSE;

  if ((req = suscan_analyzer_inspector_msg_new(
      SUSCAN_ANALYZER_INSPECTOR_MSGKIND_OPEN,
      req_id)) == NULL) {
    SU_ERROR("Failed to craft open message\n");
    goto done;
  }

  req->channel = *channel;

  if (!suscan_analyzer_write(
      analyzer,
      SUSCAN_ANALYZER_MESSAGE_TYPE_INSPECTOR,
      req)) {
    SU_ERROR("Failed to send open command\n");
    goto done;
  }

  req = NULL; /* Now it belongs to the queue */

  ok = SU_TRUE;

done:
  if (req != NULL)
    suscan_analyzer_inspector_msg_destroy(req);

  return ok;
}

SUHANDLE
suscan_inspector_open(
    suscan_analyzer_t *analyzer,
    const struct sigutils_channel *channel)
{
  struct suscan_analyzer_inspector_msg *resp = NULL;
  uint32_t req_id = rand();
  SUHANDLE handle = -1;

  SU_TRYCATCH(
      suscan_inspector_open_async(analyzer, channel, req_id),
      goto done);

  SU_TRYCATCH(
      resp = suscan_analyzer_read_inspector_msg(analyzer),
      goto done);

  if (resp->req_id != req_id) {
    SU_ERROR("Unmatched response received\n");
    goto done;
  } else if (resp->kind != SUSCAN_ANALYZER_INSPECTOR_MSGKIND_OPEN) {
    SU_ERROR("Unexpected message kind\n");
    goto done;
  }

  handle = resp->handle;

done:
  if (resp != NULL)
    suscan_analyzer_inspector_msg_destroy(resp);

  return handle;
}

SUBOOL
suscan_inspector_close_async(
    suscan_analyzer_t *analyzer,
    SUHANDLE handle,
    uint32_t req_id)
{
  struct suscan_analyzer_inspector_msg *req = NULL;
  uint32_t type;
  SUBOOL ok = SU_FALSE;

  if ((req = suscan_analyzer_inspector_msg_new(
      SUSCAN_ANALYZER_INSPECTOR_MSGKIND_CLOSE,
      req_id)) == NULL) {
    SU_ERROR("Failed to craft close message\n");
    goto done;
  }
  req->handle = handle;

  if (!suscan_analyzer_write(
      analyzer,
      SUSCAN_ANALYZER_MESSAGE_TYPE_INSPECTOR,
      req)) {
    SU_ERROR("Failed to send close command\n");
    goto done;
  }

  req = NULL;

  ok = SU_TRUE;

done:
  if (req != NULL)
    suscan_analyzer_inspector_msg_destroy(req);

  return ok;
}

SUBOOL
suscan_inspector_close(
    suscan_analyzer_t *analyzer,
    SUHANDLE handle)
{

  struct suscan_analyzer_inspector_msg *resp = NULL;
  uint32_t req_id = rand();
  SUBOOL ok = SU_FALSE;

  SU_TRYCATCH(
      suscan_inspector_close_async(analyzer, handle, req_id),
      goto done);

  SU_TRYCATCH(
      resp = suscan_analyzer_read_inspector_msg(analyzer),
      goto done);

  if (resp->req_id != req_id) {
    SU_ERROR("Unmatched response received\n");
    goto done;
  }

  if (resp->kind == SUSCAN_ANALYZER_INSPECTOR_MSGKIND_WRONG_HANDLE) {
    SU_WARNING("Wrong handle passed to analyzer\n");
    goto done;
  } else if (resp->kind != SUSCAN_ANALYZER_INSPECTOR_MSGKIND_CLOSE) {
    SU_ERROR("Unexpected message kind\n");
    goto done;
  }

  ok = SU_TRUE;

done:
  if (resp != NULL)
    suscan_analyzer_inspector_msg_destroy(resp);

  return ok;
}

SUBOOL
suscan_inspector_get_info_async(
    suscan_analyzer_t *analyzer,
    SUHANDLE handle,
    uint32_t req_id)
{
  struct suscan_analyzer_inspector_msg *req = NULL;
  uint32_t type;
  SUBOOL ok = SU_FALSE;

  if ((req = suscan_analyzer_inspector_msg_new(
      SUSCAN_ANALYZER_INSPECTOR_MSGKIND_GET_INFO,
      req_id)) == NULL) {
    SU_ERROR("Failed to craft get_info message\n");
    goto done;
  }

  req->handle = handle;

  if (!suscan_analyzer_write(
      analyzer,
      SUSCAN_ANALYZER_MESSAGE_TYPE_INSPECTOR,
      req)) {
    SU_ERROR("Failed to send get_info command\n");
    goto done;
  }

  req = NULL;

  ok = SU_TRUE;

done:
  if (req != NULL)
    suscan_analyzer_inspector_msg_destroy(req);

  return ok;
}

SUBOOL
suscan_inspector_get_info(
    suscan_analyzer_t *analyzer,
    SUHANDLE handle,
    struct suscan_baud_det_result *result)
{
  struct suscan_analyzer_inspector_msg *resp = NULL;
  uint32_t req_id = rand();
  SUBOOL ok = SU_FALSE;

  SU_TRYCATCH(
      suscan_inspector_get_info_async(analyzer, handle, req_id),
      goto done);

  SU_TRYCATCH(
      resp = suscan_analyzer_read_inspector_msg(analyzer),
      goto done);

  if (resp->req_id != req_id) {
    SU_ERROR("Unmatched response received\n");
    goto done;
  }

  if (resp->kind == SUSCAN_ANALYZER_INSPECTOR_MSGKIND_WRONG_HANDLE) {
    SU_WARNING("Wrong handle passed to analyzer\n");
    goto done;
  } else if (resp->kind != SUSCAN_ANALYZER_INSPECTOR_MSGKIND_INFO) {
    SU_ERROR("Unexpected message kind %d\n", resp->kind);
    goto done;
  }

  *result = resp->baud;

  ok = SU_TRUE;

done:
  if (resp != NULL)
    suscan_analyzer_inspector_msg_destroy(resp);

  return ok;
}

SUBOOL
suscan_inspector_set_params_async(
    suscan_analyzer_t *analyzer,
    SUHANDLE handle,
    const struct suscan_inspector_params *params,
    uint32_t req_id)
{
  struct suscan_analyzer_inspector_msg *req = NULL;
  uint32_t type;
  SUBOOL ok = SU_FALSE;

  if ((req = suscan_analyzer_inspector_msg_new(
      SUSCAN_ANALYZER_INSPECTOR_MSGKIND_PARAMS,
      req_id)) == NULL) {
    SU_ERROR("Failed to craft get_info message\n");
    goto done;
  }

  req->handle = handle;
  req->params = *params;

  if (!suscan_analyzer_write(
      analyzer,
      SUSCAN_ANALYZER_MESSAGE_TYPE_INSPECTOR,
      req)) {
    SU_ERROR("Failed to send set_params command\n");
    goto done;
  }

  req = NULL;

  ok = SU_TRUE;

done:
  if (req != NULL)
    suscan_analyzer_inspector_msg_destroy(req);

  return ok;
}
