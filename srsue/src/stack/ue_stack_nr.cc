/*
 * Copyright 2013-2020 Software Radio Systems Limited
 *
 * This file is part of srsLTE.
 *
 * srsLTE is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of
 * the License, or (at your option) any later version.
 *
 * srsLTE is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * A copy of the GNU Affero General Public License can be found in
 * the LICENSE file in the top-level directory of this distribution
 * and at http://www.gnu.org/licenses/.
 *
 */

#include "srsue/hdr/stack/ue_stack_nr.h"
#include "srslte/srslte.h"

using namespace srslte;

namespace srsue {

ue_stack_nr::ue_stack_nr(srslte::logger* logger_) :
  logger(logger_),
  timers(64),
  thread("STACK"),
  pending_tasks(1024),
  background_tasks(2),
  rlc_log("RLC"),
  pdcp_log("PDCP"),
  pool_log("POOL")
{
  mac.reset(new mac_nr());
  pdcp.reset(new srslte::pdcp(this, "PDCP"));
  rlc.reset(new srslte::rlc("RLC"));
  rrc.reset(new rrc_nr());

  // setup logging for pool, RLC and PDCP
  pool_log->set_level(srslte::LOG_LEVEL_ERROR);
  byte_buffer_pool::get_instance()->set_log(pool_log.get());

  ue_queue_id         = pending_tasks.add_queue();
  sync_queue_id       = pending_tasks.add_queue();
  gw_queue_id         = pending_tasks.add_queue();
  mac_queue_id        = pending_tasks.add_queue();
  background_queue_id = pending_tasks.add_queue();

  background_tasks.start();
}

ue_stack_nr::~ue_stack_nr()
{
  stop();
}

std::string ue_stack_nr::get_type()
{
  return "nr";
}

int ue_stack_nr::init(const stack_args_t& args_, phy_interface_stack_nr* phy_, gw_interface_stack* gw_)
{
  phy = phy_;
  gw  = gw_;
  return init(args_);
}

int ue_stack_nr::init(const stack_args_t& args_)
{
  args = args_;

  srslte::logmap::register_log(std::unique_ptr<srslte::log>{new srslte::log_filter{"MAC", logger, true}});

  srslte::log_ref mac_log{"MAC"};
  mac_log->set_level(args.log.mac_level);
  mac_log->set_hex_limit(args.log.mac_hex_limit);
  rlc_log->set_level(args.log.rlc_level);
  rlc_log->set_hex_limit(args.log.rlc_hex_limit);
  pdcp_log->set_level(args.log.pdcp_level);
  pdcp_log->set_hex_limit(args.log.pdcp_hex_limit);

  mac_nr_args_t mac_args = {};
  mac_args.pcap          = args.pcap;
  mac_args.drb_lcid      = 4;
  mac->init(mac_args, phy, rlc.get(), &timers, this);
  rlc->init(pdcp.get(), rrc.get(), &timers, 0 /* RB_ID_SRB0 */);
  pdcp->init(rlc.get(), rrc.get(), gw);

  // TODO: where to put RRC args?
  rrc_nr_args_t rrc_args     = {};
  rrc_args.log_level         = args.log.rrc_level;
  rrc_args.log_hex_limit     = args.log.rrc_hex_limit;
  rrc_args.coreless.drb_lcid = 4;
  rrc_args.coreless.ip_addr  = "192.168.1.3";
  rrc->init(phy, mac.get(), rlc.get(), pdcp.get(), gw, &timers, this, rrc_args);

  // statically setup TUN (will be done through RRC later)
  char* err_str = nullptr;
  if (gw->setup_if_addr(rrc_args.coreless.drb_lcid,
                        LIBLTE_MME_PDN_TYPE_IPV4,
                        htonl(inet_addr(rrc_args.coreless.ip_addr.c_str())),
                        nullptr,
                        err_str)) {
    printf("Error configuring TUN interface\n");
  }

  running = true;
  start(STACK_MAIN_THREAD_PRIO);

  return SRSLTE_SUCCESS;
}

void ue_stack_nr::stop()
{
  if (running) {
    pending_tasks.try_push(ue_queue_id, [this]() { stop_impl(); });
    wait_thread_finish();
  }
}

void ue_stack_nr::stop_impl()
{
  running = false;

  rrc->stop();

  rlc->stop();
  pdcp->stop();
  mac->stop();

  if (mac_pcap != nullptr) {
    mac_pcap.reset();
  }
}

bool ue_stack_nr::switch_on()
{
  return true;
}

bool ue_stack_nr::switch_off()
{
  return true;
}

bool ue_stack_nr::get_metrics(stack_metrics_t* metrics)
{
  // mac.get_metrics(metrics->mac);
  rlc->get_metrics(metrics->rlc);
  // rrc.get_metrics(metrics->rrc);
  return true;
}

void ue_stack_nr::run_thread()
{
  while (running) {
    srslte::move_task_t task{};
    if (pending_tasks.wait_pop(&task) >= 0) {
      task();
    }
  }
}

/***********************************************************************************************************************
 *                                                Stack Interfaces
 **********************************************************************************************************************/

/********************
 *   GW Interface
 *******************/

/**
 * Push GW SDU to stack
 * @param lcid
 * @param sdu
 * @param blocking
 */
void ue_stack_nr::write_sdu(uint32_t lcid, srslte::unique_byte_buffer_t sdu, bool blocking)
{
  if (pdcp != nullptr) {
    std::pair<bool, move_task_t> ret = pending_tasks.try_push(
        gw_queue_id,
        std::bind([this, lcid, blocking](
                      srslte::unique_byte_buffer_t& sdu) { pdcp->write_sdu(lcid, std::move(sdu), blocking); },
                  std::move(sdu)));
    if (not ret.first) {
      pdcp_log->warning("GW SDU with lcid=%d was discarded.\n", lcid);
    }
  }
}

/********************
 *  SYNC Interface
 *******************/

/**
 * Sync thread signal that it is in sync
 */
void ue_stack_nr::in_sync()
{
  // pending_tasks.push(sync_queue_id, task_t{[this](task_t*) { rrc.in_sync(); }});
}

void ue_stack_nr::out_of_sync()
{
  // pending_tasks.push(sync_queue_id, task_t{[this](task_t*) { rrc.out_of_sync(); }});
}

void ue_stack_nr::run_tti(uint32_t tti)
{
  pending_tasks.push(sync_queue_id, [this, tti]() { run_tti_impl(tti); });
}

void ue_stack_nr::run_tti_impl(uint32_t tti)
{
  mac->run_tti(tti);
  rrc->run_tti(tti);
  timers.step_all();
}

/********************
 * low MAC Interface
 *******************/

void ue_stack_nr::start_cell_search()
{
  // not implemented
}

void ue_stack_nr::start_cell_select(const phy_interface_rrc_lte::phy_cell_t* cell)
{
  // not implemented
}

/***************************
 * Task Handling Interface
 **************************/

void ue_stack_nr::enqueue_background_task(std::function<void(uint32_t)> f)
{
  background_tasks.push_task(std::move(f));
}

void ue_stack_nr::notify_background_task_result(srslte::move_task_t task)
{
  // run the notification in the stack thread
  pending_tasks.push(background_queue_id, std::move(task));
}

void ue_stack_nr::defer_callback(uint32_t duration_ms, std::function<void()> func)
{
  timers.defer_callback(duration_ms, func);
}

void ue_stack_nr::defer_task(srslte::move_task_t task)
{
  deferred_stack_tasks.push_back(std::move(task));
}

} // namespace srsue
