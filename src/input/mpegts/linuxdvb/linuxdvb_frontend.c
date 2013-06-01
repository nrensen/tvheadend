/*
 *  Tvheadend - Linux DVB frontend
 *
 *  Copyright (C) 2013 Adam Sutton
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "tvheadend.h"
#include "linuxdvb_private.h"

#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <linux/dvb/dmx.h>

static int
linuxdvb_frontend_tune
  ( linuxdvb_frontend_t *lfe, linuxdvb_mux_t *lm );
static void
linuxdvb_frontend_monitor ( void *aux );
static void *
linuxdvb_frontend_input_thread ( void *aux );

/* **************************************************************************
 * Class definition
 * *************************************************************************/

extern const idclass_t linuxdvb_hardware_class;

static const char *
linuxdvb_frontend_class_get_title ( idnode_t *in )
{
  linuxdvb_frontend_t *lfe = (linuxdvb_frontend_t*)in;
  if (lfe->lh_displayname)
    return lfe->lh_displayname;
  if (lfe->lfe_fe_path)
    return lfe->lfe_fe_path;
  return "unknown";
}

static void
linuxdvb_frontend_class_save ( idnode_t *in )
{
  linuxdvb_frontend_t *lfe = (linuxdvb_frontend_t*)in;
  linuxdvb_device_save((linuxdvb_device_t*)lfe->lh_parent->lh_parent);
}

const idclass_t linuxdvb_frontend_class =
{
  .ic_super      = &linuxdvb_hardware_class,
  .ic_class      = "linuxdvb_frontend",
  .ic_caption    = "Linux DVB Frontend",
  .ic_get_title  = linuxdvb_frontend_class_get_title,
  .ic_save       = linuxdvb_frontend_class_save,
  .ic_properties = (const property_t[]) {
    { PROPDEF2("fe_path", "Frontend Path",
               PT_STR, linuxdvb_frontend_t, lfe_fe_path, 1) },
    { PROPDEF2("dvr_path", "Input Path",
               PT_STR, linuxdvb_frontend_t, lfe_dvr_path, 1) },
    { PROPDEF2("dmx_path", "Demux Path",
               PT_STR, linuxdvb_frontend_t, lfe_dmx_path, 1) },
    { PROPDEF2("number", "FE Number",
               PT_INT, linuxdvb_frontend_t, lfe_number, 1) },
    {}
  }
};

const idclass_t linuxdvb_frontend_dvbt_class =
{
  .ic_super      = &linuxdvb_frontend_class,
  .ic_class      = "linuxdvb_frontend_dvbt",
  .ic_caption    = "Linux DVB-T Frontend",
  .ic_properties = (const property_t[]){
    {}
  }
};

const idclass_t linuxdvb_frontend_dvbs_class =
{
  .ic_super      = &linuxdvb_frontend_class,
  .ic_class      = "linuxdvb_frontend_dvbs",
  .ic_caption    = "Linux DVB-S Frontend",
  .ic_properties = (const property_t[]){
    {}
  }
};

const idclass_t linuxdvb_frontend_dvbc_class =
{
  .ic_super      = &linuxdvb_frontend_class,
  .ic_class      = "linuxdvb_frontend_dvbc",
  .ic_caption    = "Linux DVB-C Frontend",
  .ic_properties = (const property_t[]){
    {}
  }
};

const idclass_t linuxdvb_frontend_atsc_class =
{
  .ic_super      = &linuxdvb_frontend_class,
  .ic_class      = "linuxdvb_frontend_atsc",
  .ic_caption    = "Linux ATSC Frontend",
  .ic_properties = (const property_t[]){
    {}
  }
};

/* **************************************************************************
 * Class methods
 * *************************************************************************/

static int
linuxdvb_frontend_is_enabled ( mpegts_input_t *mi )
{
  linuxdvb_frontend_t *lfe = (linuxdvb_frontend_t*)mi;
  if (lfe->lfe_fe_path == NULL) return 0;
  if (!lfe->mi_enabled) return 0;
  if (access(lfe->lfe_fe_path, R_OK | W_OK)) return 0;
  return 1;
}

static void
linuxdvb_frontend_display_name ( mpegts_input_t* mi, char *buf, size_t len )
{
  strncpy(buf, linuxdvb_frontend_class_get_title(&mi->mi_id), len);
}

#if 0
static int
linuxdvb_frontend_is_free ( mpegts_input_t *mi )
{
#if 0
  linuxdvb_frontend_t *lfe = (linuxdvb_frontend_t*)mi;
  linuxdvb_adapter_t  *la =  lfe->lfe_adapter;
  return linuxdvb_adapter_is_free(la);
#endif
  return 0;
}

static int
linuxdvb_frontend_current_weight ( mpegts_input_t *mi )
{
#if 0
  linuxdvb_frontend_t *lfe = (linuxdvb_frontend_t*)mi;
  linuxdvb_adapter_t  *la =  lfe->lfe_adapter;
  return linuxdvb_adapter_current_weight(la);
#endif
  return 0;
}
#endif

static void
linuxdvb_frontend_stop_mux
  ( mpegts_input_t *mi, mpegts_mux_instance_t *mmi )
{
  linuxdvb_frontend_t *lfe = (linuxdvb_frontend_t*)mi;

  /* Stop thread */
  if (lfe->lfe_dvr_pipe.wr > 0) {
    tvh_write(lfe->lfe_dvr_pipe.wr, "", 1);
    pthread_join(lfe->lfe_dvr_thread, NULL);
    tvh_pipe_close(&lfe->lfe_dvr_pipe);
    tvhlog(LOG_DEBUG, "linuxdvb", "stopped dvr thread");
  }
}

static int
linuxdvb_frontend_start_mux
  ( mpegts_input_t *mi, mpegts_mux_instance_t *mmi )
{
  int r;
  linuxdvb_frontend_t *lfe = (linuxdvb_frontend_t*)mi;
  mpegts_mux_instance_t *cur = LIST_FIRST(&mi->mi_mux_active);
  tvhtrace("mpegts", "linuxdvb_frontend_start_mux(%p, %p)", mi, mmi);

  // Not sure if this is right place?
  /* Currently active */
  if (cur != NULL) {

    /* Already tuned */
    if (mmi == cur)
      return 0;

    /* Stop current */
    cur->mmi_mux->mm_stop(cur->mmi_mux);
  }
  assert(LIST_FIRST(&mi->mi_mux_active) == NULL);

  /* Open FE */
  if (lfe->lfe_fe_fd <= 0) {
    lfe->lfe_fe_fd = tvh_open(lfe->lfe_fe_path, O_RDWR | O_NONBLOCK, 0);
    if (lfe->lfe_fe_fd <= 0) {
      return SM_CODE_TUNING_FAILED;
    }
  }

  /* Tune */
  r = linuxdvb_frontend_tune(lfe, (linuxdvb_mux_t*)mmi->mmi_mux);

  /* Failed */
  if (r != 0) {
    tvhlog(LOG_ERR, "linuxdvb", "'%s' failed to tune '%s' error %s",
           lfe->lfe_fe_path, "TODO", strerror(errno));
    if (errno == EINVAL)
      mmi->mmi_tune_failed = 1;
    return SM_CODE_TUNING_FAILED;
  }

  /* Start monitor */
  time(&lfe->lfe_monitor);
  lfe->lfe_monitor += 4;
  gtimer_arm_ms(&lfe->lfe_monitor_timer, linuxdvb_frontend_monitor, lfe, 50);
  
  /* Send alert */
  // TODO: should this be moved elsewhere?
  return r;
}

static void
linuxdvb_frontend_open_service
  ( mpegts_input_t *mi, mpegts_service_t *ms )
{
}

static void
linuxdvb_frontend_close_service
  ( mpegts_input_t *mi, mpegts_service_t *ms )
{
}

/* **************************************************************************
 * Data processing
 * *************************************************************************/

static void *
linuxdvb_frontend_input_thread ( void *aux )
{
  linuxdvb_frontend_t *lfe = aux;
  mpegts_mux_instance_t *mmi;
  int dmx = -1, dvr = -1;
  char buf[256];
  uint8_t tsb[18800];
  int pos = 0, nfds, efd;
  ssize_t c;
  struct epoll_event ev;
  struct dmx_pes_filter_params dmx_param;

  /* Get MMI */
  pthread_mutex_lock(&global_lock);
  lfe->mi_display_name((mpegts_input_t*)lfe, buf, sizeof(buf));
  mmi = LIST_FIRST(&lfe->mi_mux_active);
  pthread_mutex_unlock(&global_lock);
  if (mmi == NULL) return NULL;

  /* Open DMX */
  dmx = tvh_open(lfe->lfe_dmx_path, O_RDWR, 0);
  if (dmx < 0) {
    tvherror("linuxdvb", "%s - failed to open %s", buf, lfe->lfe_dmx_path);
    return NULL;
  }
  memset(&dmx_param, 0, sizeof(dmx_param));
  dmx_param.pid      = 0x2000;
  dmx_param.input    = DMX_IN_FRONTEND;
  dmx_param.output   = DMX_OUT_TS_TAP;
  dmx_param.pes_type = DMX_PES_OTHER;
  dmx_param.flags    = DMX_IMMEDIATE_START;
  if(ioctl(dmx, DMX_SET_PES_FILTER, &dmx_param) == -1) {
    tvhlog(LOG_ERR, "dvb",
    "Unable to configure demuxer \"%s\" for all PIDs -- %s",
    lfe->lfe_dmx_path, strerror(errno));
    close(dmx);
    return NULL;
  }

  /* Open DVR */
  dvr = tvh_open(lfe->lfe_dvr_path, O_RDONLY | O_NONBLOCK, 0);
  if (dvr < 0) {
    close(dmx);
    tvherror("linuxdvb", "%s - failed to open %s", buf, lfe->lfe_dvr_path);
    return NULL;
  }

  /* Setup poll */
  efd = epoll_create(2);
  memset(&ev, 0, sizeof(ev));
  ev.events  = EPOLLIN;
  ev.data.fd = dvr;
  epoll_ctl(efd, EPOLL_CTL_ADD, ev.data.fd, &ev);
  ev.events  = EPOLLIN;
  ev.data.fd = lfe->lfe_dvr_pipe.rd;
  epoll_ctl(efd, EPOLL_CTL_ADD, ev.data.fd, &ev);

  /* Read */
  while (1) {
    nfds = epoll_wait(efd, &ev, 1, 10);
    if (nfds < 1) continue;
    if (ev.data.fd != dvr) break;
    
    /* Read */
    c = read(dvr, tsb+pos, sizeof(tsb)-pos);
    if (c < 0) {
      if ((errno == EAGAIN) || (errno == EINTR))
        continue;
      if (errno == EOVERFLOW) {
        tvhlog(LOG_WARNING, "linuxdvb", "%s - read() EOVERFLOW", buf);
        continue;
      }
      tvhlog(LOG_ERR, "linuxdvb", "%s - read() error %d (%s)",
             buf, errno, strerror(errno));
      break;
    }
    
    /* Process */
    pos = mpegts_input_recv_packets((mpegts_input_t*)lfe, mmi, tsb, c,
                                    NULL, NULL, buf);
  }

  close(dmx);
  close(dvr);
  return NULL;
}

static void
linuxdvb_frontend_monitor ( void *aux )
{
  linuxdvb_frontend_t *lfe = aux;
  mpegts_mux_instance_t *mmi = LIST_FIRST(&lfe->mi_mux_active);
  mpegts_mux_t *mm;

  if (!mmi) return;
  mm = mmi->mmi_mux;
  
  fe_status_t fe_status;

  if (!ioctl(lfe->lfe_fe_fd, FE_READ_STATUS, &fe_status))
    tvhtrace("mpegts", "fe_status = %02X", fe_status); 
  else
    tvhtrace("mpegts", "fe_status = unknown");

  if (fe_status & FE_HAS_LOCK) {
    // Note: the lock
    // Open pending services
    tvh_pipe(O_NONBLOCK, &lfe->lfe_dvr_pipe);
    pthread_create(&lfe->lfe_dvr_thread, NULL, linuxdvb_frontend_input_thread, lfe);

    // TODO: these tables need to vary based on type
    mpegts_table_add(mm, DVB_PAT_BASE, DVB_PAT_MASK, dvb_pat_callback,
                     NULL, "pat", MT_QUICKREQ | MT_CRC, DVB_PAT_PID);
    mpegts_table_add(mm, DVB_SDT_BASE, DVB_SDT_MASK, dvb_sdt_callback,
                     NULL, "sdt", MT_QUICKREQ | MT_CRC, DVB_SDT_PID);
    mpegts_table_add(mm, DVB_NIT_BASE, DVB_NIT_MASK, dvb_nit_callback,
                     NULL, "nit", MT_QUICKREQ | MT_CRC, DVB_NIT_PID);
    mpegts_table_add(mm, DVB_BAT_BASE, DVB_BAT_MASK, dvb_bat_callback,
                     NULL, "bat", MT_CRC, DVB_BAT_PID);
  } else {
    gtimer_arm_ms(&lfe->lfe_monitor_timer, linuxdvb_frontend_monitor, lfe, 1000);
  }
}


static int
linuxdvb_frontend_tune
  ( linuxdvb_frontend_t *lfe, linuxdvb_mux_t *lm )
{
  int r;
  struct dvb_frontend_event ev;
  dvb_mux_conf_t *dmc = &lm->lm_tuning;
  struct dvb_frontend_parameters *p = &dmc->dmc_fe_params;

  /* S2 tuning */
#if DVB_API_VERSION >= 5
  struct dtv_property cmds[20];
  struct dtv_properties cmdseq = { .num = 0, .props = cmds };
  
  /* Clear Q */
  static struct dtv_property clear_p[] = {
    { .cmd = DTV_CLEAR },
  };
  static struct dtv_properties clear_cmdseq = {
    .num = 1,
    .props = clear_p
  };
  if ((ioctl(lfe->lfe_fe_fd, FE_SET_PROPERTY, &clear_cmdseq)) != 0)
    return -1;
  
  /* Tune */
#define S2CMD(c, d)\
  cmds[cmdseq.num].cmd      = c;\
  cmds[cmdseq.num++].u.data = d
  S2CMD(DTV_DELIVERY_SYSTEM, lm->lm_tuning.dmc_fe_delsys);
  S2CMD(DTV_FREQUENCY,       p->frequency);
  S2CMD(DTV_INVERSION,       p->inversion);

  /* DVB-T */
  if (lfe->lfe_info.type == FE_OFDM) {
    S2CMD(DTV_BANDWIDTH_HZ,      dvb_bandwidth(p->u.ofdm.bandwidth));
    S2CMD(DTV_CODE_RATE_HP,      p->u.ofdm.code_rate_HP);
    S2CMD(DTV_CODE_RATE_LP,      p->u.ofdm.code_rate_LP);
    S2CMD(DTV_MODULATION,        p->u.ofdm.constellation);
    S2CMD(DTV_TRANSMISSION_MODE, p->u.ofdm.transmission_mode);
    S2CMD(DTV_GUARD_INTERVAL,    p->u.ofdm.guard_interval);
    S2CMD(DTV_HIERARCHY,         p->u.ofdm.hierarchy_information);

  /* DVB-C */
  } else if (lfe->lfe_info.type == FE_QAM) {
    S2CMD(DTV_SYMBOL_RATE,       p->u.qam.symbol_rate);
    S2CMD(DTV_MODULATION,        p->u.qam.modulation);
    S2CMD(DTV_INNER_FEC,         p->u.qam.fec_inner);

  /* DVB-S */
  } else if (lfe->lfe_info.type == FE_QPSK) {
    S2CMD(DTV_SYMBOL_RATE,       p->u.qpsk.symbol_rate);
    S2CMD(DTV_INNER_FEC,         p->u.qpsk.fec_inner);
    S2CMD(DTV_MODULATION,        dmc->dmc_fe_modulation);
    S2CMD(DTV_ROLLOFF,           dmc->dmc_fe_rolloff);

  /* ATSC */
  } else {
    S2CMD(DTV_MODULATION,        p->u.vsb.modulation);
  }

  /* Tune */
  S2CMD(DTV_TUNE, 0);
#undef S2CMD
#endif

  /* discard stale events */
  while (1) {
    if (ioctl(lfe->lfe_fe_fd, FE_GET_EVENT, &ev) == -1)
      break;
  }

  /* S2 tuning */
#if DVB_API_VERSION >= 5
  r = ioctl(lfe->lfe_fe_fd, FE_SET_PROPERTY, &cmdseq);

  /* v3 tuning */
#else
  r = ioctl(lfe->lfe_fe_fd, FE_SET_FRONTEND, p);
#endif

  return r;
}

/* **************************************************************************
 * Creation/Config
 * *************************************************************************/
 
linuxdvb_frontend_t *
linuxdvb_frontend_create0
  ( linuxdvb_adapter_t *la, const char *uuid, htsmsg_t *conf, fe_type_t type )
{
  const char *str;
  const idclass_t *idc;
  pthread_t tid;

  /* Get type */
  if (conf) {
    if (!(str = htsmsg_get_str(conf, "type")))
      return NULL;
    type = dvb_str2type(str);
  }

  /* Class */
  if (type == FE_QPSK)
    idc = &linuxdvb_frontend_dvbs_class;
  else if (type == FE_QAM)
    idc = &linuxdvb_frontend_dvbc_class;
  else if (type == FE_OFDM)
    idc = &linuxdvb_frontend_dvbt_class;
  else if (type == FE_ATSC)
    idc = &linuxdvb_frontend_atsc_class;
  else {
    tvherror("linuxdvb", "unknown FE type %d", type);
    return NULL;
  }

  linuxdvb_frontend_t *lfe
    = (linuxdvb_frontend_t*)
        mpegts_input_create0(calloc(1, sizeof(linuxdvb_frontend_t)),
                             idc, uuid, conf);
  lfe->lfe_info.type = type;

  /* Input callbacks */
  lfe->mi_is_enabled     = linuxdvb_frontend_is_enabled;
  lfe->mi_display_name   = linuxdvb_frontend_display_name;
  lfe->mi_start_mux      = linuxdvb_frontend_start_mux;
  lfe->mi_stop_mux       = linuxdvb_frontend_stop_mux;
  lfe->mi_open_service   = linuxdvb_frontend_open_service;
  lfe->mi_close_service  = linuxdvb_frontend_close_service;

  /* Adapter link */
  lfe->lh_parent = (linuxdvb_hardware_t*)la;
  LIST_INSERT_HEAD(&la->lh_children, (linuxdvb_hardware_t*)lfe, lh_parent_link);
 
  /* Start table thread */
  pthread_create(&tid, NULL, mpegts_input_table_thread, lfe);

  /* No conf */
  if (!conf)
    return lfe;

  /* TODO: this should be done differently */
  if ((str = htsmsg_get_str(conf, "network"))) {
    linuxdvb_network_t *ln = linuxdvb_network_find_by_uuid(str);
    if (ln) {
      if (ln->ln_type == lfe->lfe_info.type) {
        mpegts_network_add_input((mpegts_network_t*)ln, (mpegts_input_t*)lfe);
      } else
        tvhlog(LOG_WARNING, "linuxdvb",
               "attempt to add network %s of wrong type %s to %s (%s)",
               dvb_type2str(ln->ln_type), ln->mn_network_name,
               lfe->lh_displayname, dvb_type2str(lfe->lfe_info.type));
    }
  }

  return lfe;
}

linuxdvb_frontend_t *
linuxdvb_frontend_added
  ( linuxdvb_adapter_t *la, int fe_num,
    const char *fe_path,
    const char *dmx_path,
    const char *dvr_path,
    const struct dvb_frontend_info *fe_info )
{
  linuxdvb_hardware_t *lh;
  linuxdvb_frontend_t *lfe = NULL;

  /* Find existing */
  LIST_FOREACH(lh, &la->lh_children, lh_parent_link) {
    lfe = (linuxdvb_frontend_t*)lh;
    if (lfe->lfe_number == fe_num) {
      if (lfe->lfe_info.type != fe_info->type) {
        tvhlog(LOG_ERR, "linuxdvb", "detected incorrect fe_type %s != %s",
               dvb_type2str(lfe->lfe_info.type), dvb_type2str(fe_info->type));
        return NULL;
      }
      break;
    }
  }

  /* Create new */
  if (!lfe) {
    if (!(lfe = linuxdvb_frontend_create0(la, NULL, NULL, fe_info->type))) {
      tvhlog(LOG_ERR, "linuxdvb", "failed to create frontend");
      return NULL;
    }
  }

  /* Copy info */
  lfe->lfe_number = fe_num;
  memcpy(&lfe->lfe_info, fe_info, sizeof(struct dvb_frontend_info));

  /* Set paths */
  lfe->lfe_fe_path  = strdup(fe_path);
  lfe->lfe_dmx_path = strdup(dmx_path);
  lfe->lfe_dvr_path = strdup(dvr_path);

  return lfe;
}

void
linuxdvb_frontend_save ( linuxdvb_frontend_t *lfe, htsmsg_t *m )
{
  mpegts_input_save((mpegts_input_t*)lfe, m);
  htsmsg_add_str(m, "type", dvb_type2str(lfe->lfe_info.type));
}

/******************************************************************************
 * Editor Configuration
 *
 * vim:sts=2:ts=2:sw=2:et
 *****************************************************************************/
