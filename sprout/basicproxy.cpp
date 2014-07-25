/**
 * @file basicproxy.cpp  BasicProxy class implementation
 *
 * Project Clearwater - IMS in the Cloud
 * Copyright (C) 2013  Metaswitch Networks Ltd
 *
 * Parts of this module were derived from GPL licensed PJSIP sample code
 * with the following copyrights.
 *   Copyright (C) 2008-2011 Teluu Inc. (http://www.teluu.com)
 *   Copyright (C) 2003-2008 Benny Prijono <benny@prijono.org>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation, either version 3 of the License, or (at your
 * option) any later version, along with the "Special Exception" for use of
 * the program along with SSL, set forth below. This program is distributed
 * in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR
 * A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details. You should have received a copy of the GNU General Public
 * License along with this program.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * The author can be reached by email at clearwater@metaswitch.com or by
 * post at Metaswitch Networks Ltd, 100 Church St, Enfield EN2 6BQ, UK
 *
 * Special Exception
 * Metaswitch Networks Ltd  grants you permission to copy, modify,
 * propagate, and distribute a work formed by combining OpenSSL with The
 * Software, or a work derivative of such a combination, even if such
 * copying, modification, propagation, or distribution would otherwise
 * violate the terms of the GPL. You must comply with the GPL in all
 * respects for all of the code used other than OpenSSL.
 * "OpenSSL" means OpenSSL toolkit software distributed by the OpenSSL
 * Project and licensed under the OpenSSL Licenses, or a work based on such
 * software and licensed under the OpenSSL Licenses.
 * "OpenSSL Licenses" means the OpenSSL License and Original SSLeay License
 * under which the OpenSSL Project distributes the OpenSSL toolkit software,
 * as those licenses appear in the file LICENSE-OPENSSL.
 */


extern "C" {
#include <pjsip.h>
#include <pjlib-util.h>
#include <pjlib.h>
#include <stdint.h>
}

#include <vector>


#include "log.h"
#include "utils.h"
#include "pjutils.h"
#include "stack.h"
#include "sproutsasevent.h"
#include "constants.h"
#include "basicproxy.h"


BasicProxy::BasicProxy(pjsip_endpoint* endpt,
                       std::string name,
                       int priority,
                       bool delay_trying) :
  _mod_proxy(this, endpt, name, priority, PJMODULE_MASK_PROXY),
  _mod_tu(this, endpt, name + "-tu", priority, PJMODULE_MASK_TU),
  _delay_trying(delay_trying)
{
}


BasicProxy::~BasicProxy()
{
}


// Callback to be called to handle incoming request outside of a
// existing transaction context.
pj_bool_t BasicProxy::on_rx_request(pjsip_rx_data* rdata)
{
  if (rdata->msg_info.msg->line.req.method.id != PJSIP_CANCEL_METHOD)
  {
    // Request is a normal transaction request.
    LOG_DEBUG("Process %.*s request",
              rdata->msg_info.msg->line.req.method.name.slen,
              rdata->msg_info.msg->line.req.method.name.ptr);
    on_tsx_request(rdata);
  }
  else
  {
    // Request is a CANCEL.
    LOG_DEBUG("Process CANCEL request");
    on_cancel_request(rdata);
  }

  return PJ_TRUE;
}


// Callback to be called to handle incoming response outside
// any transactions. This happens for example when 2xx/OK
// for INVITE is received and transaction will be destroyed
// immediately, so we need to forward the subsequent 2xx/OK
// retransmission statelessly.
pj_bool_t BasicProxy::on_rx_response(pjsip_rx_data *rdata)
{
  pjsip_tx_data *tdata;
  pjsip_response_addr res_addr;
  pjsip_via_hdr *hvia;
  pj_status_t status;

  LOG_DEBUG("Statelessly forwarding late response");

  // Only forward responses to INVITES
  if (rdata->msg_info.cseq->method.id == PJSIP_INVITE_METHOD)
  {
    // Create response to be forwarded upstream (Via will be stripped here)
    status = PJUtils::create_response_fwd(stack_data.endpt, rdata, 0, &tdata);
    if (status != PJ_SUCCESS)
    {
      // LCOV_EXCL_START
      LOG_ERROR("Error creating response, %s",
                PJUtils::pj_status_to_string(status).c_str());
      return PJ_TRUE;
      // LCOV_EXCL_STOP
    }

    // Get topmost Via header
    hvia = (pjsip_via_hdr*)pjsip_msg_find_hdr(tdata->msg, PJSIP_H_VIA, NULL);
    if (hvia == NULL)
    {
      // Invalid response! Just drop it
      pjsip_tx_data_dec_ref(tdata);
      return PJ_TRUE;
    }

    // Calculate the address to forward the response
    pj_bzero(&res_addr, sizeof(res_addr));
    res_addr.dst_host.type = pjsip_transport_get_type_from_name(&hvia->transport);
    res_addr.dst_host.flag =
                     pjsip_transport_get_flag_from_type(res_addr.dst_host.type);

    // Destination address is Via's received param
    res_addr.dst_host.addr.host = hvia->recvd_param;
    if (res_addr.dst_host.addr.host.slen == 0)
    {
      // Someone has messed up our Via header!
      res_addr.dst_host.addr.host = hvia->sent_by.host;
    }

    // Destination port is the rport
    if (hvia->rport_param != 0 && hvia->rport_param != -1)
    {
      res_addr.dst_host.addr.port = hvia->rport_param;
    }

    if (res_addr.dst_host.addr.port == 0)
    {
      // Ugh, original sender didn't put rport!
      // At best, can only send the response to the port in Via.
      res_addr.dst_host.addr.port = hvia->sent_by.port;
    }

    // Report SIP call and branch ID markers on the trail to make sure it gets
    // associated with the INVITE transaction at SAS.
    PJUtils::mark_sas_call_branch_ids(get_trail(rdata), rdata->msg_info.cid, rdata->msg_info.msg);

    // Forward response
    status = pjsip_endpt_send_response(stack_data.endpt, &res_addr, tdata, NULL, NULL);

    if (status != PJ_SUCCESS)
    {
      // LCOV_EXCL_START
      LOG_ERROR("Error forwarding response, %s",
                PJUtils::pj_status_to_string(status).c_str());
      return PJ_TRUE;
      // LCOV_EXCL_STOP
    }
  }

  return PJ_TRUE;
}


// Callback to be called to handle transmitted request.
// LCOV_EXCL_START - only needed to prevent linker error
pj_status_t BasicProxy::on_tx_request(pjsip_tx_data* tdata)
{
  return PJ_SUCCESS;
}
// LCOV_EXCL_STOP


// Callback to be called to handle transmitted response.
// LCOV_EXCL_START - only needed to prevent linker error
pj_status_t BasicProxy::on_tx_response(pjsip_tx_data* tdata)
{
  return PJ_SUCCESS;
}
// LCOV_EXCL_STOP


// Callback to be called to handle transaction state changed.
void BasicProxy::on_tsx_state(pjsip_transaction* tsx, pjsip_event* event)
{
  LOG_DEBUG("%s - tu_on_tsx_state %s, %s %s state=%s",
            tsx->obj_name,
            pjsip_role_name(tsx->role),
            pjsip_event_str(event->type),
            pjsip_event_str(event->body.tsx_state.type),
            pjsip_tsx_state_str(tsx->state));

  if (tsx->role == PJSIP_ROLE_UAS)
  {
    UASTsx* uas_tsx = (UASTsx*)get_from_transaction(tsx);
    if (uas_tsx != NULL)
    {
      uas_tsx->on_tsx_state(event);
    }
  }
  else
  {
    UACTsx* uac_tsx = (UACTsx*)get_from_transaction(tsx);
    if (uac_tsx != NULL)
    {
      uac_tsx->on_tsx_state(event);
    }
  }
}


/// Binds a UASTsx or UACTsx object to a PJSIP transaction
void BasicProxy::bind_transaction(void* uas_uac_tsx, pjsip_transaction* tsx)
{
  tsx->mod_data[_mod_tu.id()] = uas_uac_tsx;
}


/// Unbinds a UASTsx or UACTsx object from a PJSIP transaction
void BasicProxy::unbind_transaction(pjsip_transaction* tsx)
{
  tsx->mod_data[_mod_tu.id()] = NULL;
}


/// Gets the UASTsx or UACTsx object bound to a PJSIP transaction
void* BasicProxy::get_from_transaction(pjsip_transaction* tsx)
{
  return tsx->mod_data[_mod_tu.id()];
}


/// Process a transaction (that is, non-CANCEL) request
void BasicProxy::on_tsx_request(pjsip_rx_data* rdata)
{
  // Verify incoming request.
  int status_code = verify_request(rdata);
  if (status_code != PJSIP_SC_OK)
  {
    reject_request(rdata, status_code);
    return;
  }

  // Request looks sane, so create and initialize an object to handle the
  // request.
  UASTsx* uas_tsx = create_uas_tsx();
  pj_status_t status = (uas_tsx != NULL) ? uas_tsx->init(rdata) : PJ_ENOMEM;

  if (status != PJ_SUCCESS)
  {
    // LCOV_EXCL_START
    LOG_ERROR("Failed to create BasicProxy UAS transaction object, %s",
              PJUtils::pj_status_to_string(status).c_str());
    reject_request(rdata, PJSIP_SC_INTERNAL_SERVER_ERROR);
    delete uas_tsx;
    return;
    // LCOV_EXCL_STOP
  }

  // Process the request.
  uas_tsx->process_tsx_request();

  // Initializing the transaction entered its context, so exit now.
  uas_tsx->exit_context();
}


/// Process a received CANCEL request
void BasicProxy::on_cancel_request(pjsip_rx_data* rdata)
{
  pjsip_transaction *invite_uas;
  pj_str_t key;

  // Find the UAS INVITE transaction
  pjsip_tsx_create_key(rdata->tp_info.pool, &key, PJSIP_UAS_ROLE,
                       pjsip_get_invite_method(), rdata);
  invite_uas = pjsip_tsx_layer_find_tsx(&key, PJ_TRUE);
  if (!invite_uas)
  {
    // Invite transaction not found, respond to CANCEL with 481
    reject_request(rdata, PJSIP_SC_CALL_TSX_DOES_NOT_EXIST);
    return;
  }

  // Respond 200 OK to CANCEL.  Must do this statefully.
  pjsip_transaction* cancel_tsx;
  pj_status_t status = pjsip_tsx_create_uas(NULL, rdata, &cancel_tsx);
  if (status != PJ_SUCCESS)
  {
    // LCOV_EXCL_START
    reject_request(rdata, PJSIP_SC_INTERNAL_SERVER_ERROR);
    return;
    // LCOV_EXCL_STOP
  }

  // Feed the CANCEL request to the transaction.
  pjsip_tsx_recv_msg(cancel_tsx, rdata);

  // Send the 200 OK statefully.
  PJUtils::respond_stateful(stack_data.endpt, cancel_tsx, rdata, 200, NULL, NULL, NULL);

  // Send CANCEL to cancel the UAC transactions.
  // The UAS INVITE transaction will get final response when
  // we receive final response from the UAC INVITE transaction.
  UASTsx *uas_tsx = (UASTsx*)get_from_transaction(invite_uas);
  uas_tsx->process_cancel_request(rdata);

  // Unlock UAS tsx because it is locked in find_tsx()
  pj_grp_lock_release(invite_uas->grp_lock);
}


/// Proxy utility to verify incoming requests.
// Return non-zero if verification failed.
int BasicProxy::verify_request(pjsip_rx_data *rdata)
{
  // RFC 3261 Section 16.3 Request Validation

  // Before an element can proxy a request, it MUST verify the message's
  // validity.  A valid message must pass the following checks:
  //
  // 1. Reasonable Syntax
  // 2. URI scheme
  // 3. Max-Forwards
  // 4. (Optional) Loop Detection
  // 5. Proxy-Require
  // 6. Proxy-Authorization

  // 1. Reasonable Syntax.
  // This would have been checked by transport layer.

  // 2. URI scheme.
  // We support "sip:" and "tel:" URI schemes in this simple proxy.
  if (!(PJSIP_URI_SCHEME_IS_SIP(rdata->msg_info.msg->line.req.uri) ||
        PJSIP_URI_SCHEME_IS_TEL(rdata->msg_info.msg->line.req.uri)))
  {
    return PJSIP_SC_UNSUPPORTED_URI_SCHEME;
  }

  // 3. Max-Forwards.
  // Send error if Max-Forwards is 1 or lower.
  if ((rdata->msg_info.max_fwd) &&
      (rdata->msg_info.max_fwd->ivalue <= 1))
  {
    return PJSIP_SC_TOO_MANY_HOPS;
  }

  // 4. (Optional) Loop Detection.  Not checked in the BasicProxy.

  // 5. Proxy-Require.  This isn't checked in the BasicProxy, inheriting
  // classes may implement checks on this.

  // 6. Proxy-Authorization.  Not checked in the BasicProxy.

  // Check that non-ACK request has not been received on a shutting down
  // transport.  If it has then we won't be able to send a transaction
  // response, so it is better to reject immediately.
  if ((rdata->msg_info.msg->line.req.method.id != PJSIP_ACK_METHOD) &&
      (rdata->tp_info.transport != NULL) &&
      (rdata->tp_info.transport->is_shutdown))
  {
    return PJSIP_SC_SERVICE_UNAVAILABLE;
  }

  return PJSIP_SC_OK;
}


/// Rejects a request statelessly.
void BasicProxy::reject_request(pjsip_rx_data* rdata, int status_code)
{
  if (rdata->msg_info.msg->line.req.method.id != PJSIP_ACK_METHOD)
  {
    LOG_ERROR("Rejecting %.*s request with %d status code",
              rdata->msg_info.msg->line.req.method.name.slen,
              rdata->msg_info.msg->line.req.method.name.ptr,
              status_code);
    PJUtils::respond_stateless(stack_data.endpt,
                               rdata,
                               status_code,
                               NULL,
                               NULL,
                               NULL);
  }
}


/// Creates a UASTsx object.
// LCOV_EXCL_START - Overriden in UT
BasicProxy::UASTsx* BasicProxy::create_uas_tsx()
{
  return new UASTsx(this);
}
// LCOV_EXCL_STOP


/// UAS Transaction constructor
BasicProxy::UASTsx::UASTsx(BasicProxy* proxy) :
  _proxy(proxy),
  _req(NULL),
  _tsx(NULL),
  _lock(NULL),
  _trail(0),
  _targets(),
  _uac_tsx(),
  _pending_sends(0),
  _pending_responses(0),
  _best_rsp(NULL),
  _pending_destroy(false),
  _context_count(0)
{
  // Don't do any set-up that could fail in here - do that in the init method.
}


BasicProxy::UASTsx::~UASTsx()
{
  LOG_DEBUG("BasicProxy::UASTsx destructor (%p)", this);

  pj_assert(_context_count == 0);

  cancel_trying_timer();
  pthread_mutex_destroy(&_trying_timer_lock);

  if (_tsx != NULL)
  {
    // LCOV_EXCL_START
    _proxy->unbind_transaction(_tsx);
    // LCOV_EXCL_STOP
  }

  // Disconnect all UAC transactions from the UAS transaction.
  LOG_DEBUG("Disconnect UAC transactions from UAS transaction");
  for (size_t ii = 0; ii < _uac_tsx.size(); ++ii)
  {
    UACTsx* uac_tsx = _uac_tsx[ii];
    if (uac_tsx != NULL)
    {
      // LCOV_EXCL_START
      dissociate(uac_tsx);
      // LCOV_EXCL_STOP
    }
  }

  if (_req != NULL)
  {
    LOG_DEBUG("Free original request");
    pjsip_tx_data_dec_ref(_req);
    _req = NULL;
  }

  if (_best_rsp != NULL)
  {
    // The pre-built response hasn't been used, so free it.
    // LCOV_EXCL_START
    LOG_DEBUG("Free un-used best response");
    pjsip_tx_data_dec_ref(_best_rsp);
    _best_rsp = NULL;
    // LCOV_EXCL_STOP
  }

  // Delete any unactioned targets.
  while (!_targets.empty())
  {
    // LCOV_EXCL_START
    delete _targets.front();
    _targets.pop_front();
    // LCOV_EXCL_STOP
  }

  if (_lock != NULL)
  {
    pj_grp_lock_release(_lock);
    pj_grp_lock_dec_ref(_lock);
  }

  LOG_DEBUG("BasicProxy::UASTsx destructor completed");
}


/// Initializes the UASTsx object to handle proxying of the request.
pj_status_t BasicProxy::UASTsx::init(pjsip_rx_data* rdata)
{
  pj_status_t status;

  _trail = get_trail(rdata);

  // initialise deferred trying timer
  pthread_mutex_init(&_trying_timer_lock, NULL);
  pj_timer_entry_init(&_trying_timer, 0, (void*)this, &trying_timer_callback);
  _trying_timer.id = 0;

  // Do any start of transaction logging operations.
  on_tsx_start(rdata);

  status = PJUtils::create_request_fwd(stack_data.endpt, rdata, NULL, NULL, 0, &_req);
  if (status != PJ_SUCCESS)
  {
    // LCOV_EXCL_START - no UT for forcing PJSIP errors.
    on_tsx_complete();
    _pending_destroy = true;
    return status;
    // LCOV_EXCL_STOP
  }

  if (rdata->msg_info.msg->line.req.method.id != PJSIP_ACK_METHOD)
  {
    // Not an ACK message, so create a PJSIP UAS transaction for the request.
    status = create_pjsip_transaction(rdata);

    if (status != PJ_SUCCESS)
    {
      // Failed to create the PJSIP transaction for a stateful request.
      // LCOV_EXCL_START
      on_tsx_complete();
      _pending_destroy = true;
      return status;
      // LCOV_EXCL_STOP
    }

    // Feed the request to the UAS transaction to drive its state
    // out of NULL state.
    pjsip_tsx_recv_msg(_tsx, rdata);

    // Create a 408 response to use if none of the targets responds.
    pjsip_endpt_create_response(stack_data.endpt,
                                rdata,
                                PJSIP_SC_REQUEST_TIMEOUT,
                                NULL,
                                &_best_rsp);

    // If delay_trying is enabled, then don't send a 100 Trying now.
    if ((rdata->msg_info.msg->line.req.method.id == PJSIP_INVITE_METHOD) &&
        (!_proxy->_delay_trying))
    {
      // If the request is an INVITE then send the 100 Trying straight away.
      LOG_DEBUG("Send immediate 100 Trying response");
      send_response(100);
    }
    else if (!_proxy->_delay_trying)
    {
      // Send the 100 Trying after 3.5 secs if a final response hasn't been
      // sent.
      _trying_timer.id = TRYING_TIMER;
      pj_time_val delay = {(PJSIP_T2_TIMEOUT - PJSIP_T1_TIMEOUT) / 1000,
                           (PJSIP_T2_TIMEOUT - PJSIP_T1_TIMEOUT) % 1000 };
      pjsip_endpt_schedule_timer(stack_data.endpt, &(_trying_timer), &delay);
    }
  }
  else
  {
    // ACK will be forwarded statelessly, so we don't need a PJSIP transaction.
    // Enter the context of this object so the context count gets incremented.
    enter_context();
  }

  return PJ_SUCCESS;
}


/// Handle the incoming half of a transaction request.
void BasicProxy::UASTsx::process_tsx_request()
{
  // Process routing headers.
  int status_code = process_routing();
  if (status_code != PJSIP_SC_OK)
  {
    // LCOV_EXCL_START - no UT for forcing PJSIP errors.
    LOG_ERROR("Error process routing headers");
    if (_req->msg->line.req.method.id != PJSIP_ACK_METHOD)
    {
      // Send error response on UAS transaction.
      send_response(status_code);
    }
    else
    {
      // ACK request, so just discard and suicide.
      _pending_destroy = true;
    }
    return;
    // LCOV_EXCL_STOP
  }

  // If this is an ACK request, forward statelessly.  This happens if the
  // proxy record routes and this ACK is sent for 2xx response. An ACK that
  // is sent for non-2xx final response will be absorbed by transaction layer,
  // and it will not be received by on_rx_request() callback.
  if (_req->msg->line.req.method.id == PJSIP_ACK_METHOD)
  {
    // Report a SIP call ID marker on the trail to make sure it gets
    // associated with the INVITE transaction at SAS.  There's no need to
    // report the branch IDs as they won't be used for correlation.
    LOG_DEBUG("Statelessly forwarding ACK");
    PJUtils::mark_sas_call_branch_ids(_trail, PJSIP_MSG_CID_HDR(_req->msg), NULL);
    pj_status_t status = PJUtils::send_request_stateless(_req);
    if (status != PJ_SUCCESS)
    {
      // LCOV_EXCL_START - no UT for forcing PJSIP errors.
      LOG_ERROR("Error forwarding request, %s",
                PJUtils::pj_status_to_string(status).c_str());
      // LCOV_EXCL_STOP
    }

    // We should be completely done with this message (and PJSIP will have
    // freed it).
    on_tsx_complete();
    _req = NULL;
    _pending_destroy = true;
    return;
  }

  if (_targets.size() == 0)
  {
    // We don't have any targets yet, so calculate them now.
    status_code = calculate_targets();
    if (status_code != PJSIP_SC_OK)
    {
      LOG_DEBUG("Calculate targets failed with %d status code", status_code);
      send_response(status_code);
      return;
    }
  }

  if (_targets.size() == 0)
  {
    // No targets found, so reject with a 404 status code.  Should never
    // happen as calculate_targets should return a status code if it
    // doesn't add any targets.
    // LCOV_EXCL_START - Should never happen.
    LOG_INFO("Reject request with 404");
    send_response(PJSIP_SC_NOT_FOUND);
    return;
    // LCOV_EXCL_STOP
  }

  // Now set up the data structures and transactions required to
  // process the request and send it.
  pj_status_t status = forward_to_targets();

  if (status != PJ_SUCCESS)
  {
    // Send 500/Internal Server Error to UAS transaction
    // LCOV_EXCL_START
    LOG_ERROR("Failed to allocate UAC transaction for UAS transaction");
    send_response(PJSIP_SC_INTERNAL_SERVER_ERROR);
    return;
    // LCOV_EXCL_STOP
  }
}


/// Handle a received CANCEL request.
void BasicProxy::UASTsx::process_cancel_request(pjsip_rx_data* rdata)
{
  LOG_DEBUG("%s - Cancel for UAS transaction", name());

  // Send CANCEL to cancel the UAC transactions.
  // The UAS INVITE transaction will get final response when
  // we receive final response from the UAC INVITE transaction.
  cancel_pending_uac_tsx(0, false);
}


/// Process route information in the request
int BasicProxy::UASTsx::process_routing()
{
  pjsip_msg* msg = _req->msg;
  pjsip_sip_uri* req_uri = (pjsip_sip_uri*)msg->line.req.uri;
  pjsip_route_hdr* hroute;

  // RFC 3261 Section 16.4 Route Information Preprocessing


  // The proxy MUST inspect the Request-URI of the request.  If the
  // Request-URI of the request contains a value this proxy previously
  // placed into a Record-Route header field (see Section 16.6 item 4),
  // the proxy MUST replace the Request-URI in the request with the last
  // value from the Route header field, and remove that value from the
  // Route header field.  The proxy MUST then proceed as if it received
  // this modified request.
  if (PJUtils::is_uri_local((pjsip_uri*)req_uri))
  {
    pjsip_route_hdr *r;
    pjsip_sip_uri *uri;

    // Find the first Route header
    r = hroute = (pjsip_route_hdr*)pjsip_msg_find_hdr(msg,
                                                      PJSIP_H_ROUTE,
                                                      NULL);
    if (r == NULL)
    {
      // No Route header. This request is destined for this proxy.
      return PJSIP_SC_OK;
    }

    // Find the last Route header
    while ((r = (pjsip_route_hdr*)pjsip_msg_find_hdr(msg,
                                                     PJSIP_H_ROUTE,
                                                     r->next)) != NULL )
    {
      hroute = r;
    }

    // If the last Route header doesn't have ";lr" parameter, then
    // this is a strict-routed request indeed, and we follow the steps
    // in processing strict-route requests above.
    //
    // But if it does contain ";lr" parameter, skip the strict-route
    // processing.
    uri = (pjsip_sip_uri*)pjsip_uri_get_uri(&hroute->name_addr);
    if (uri->lr_param == 0)
    {
      // Yes this is strict route, so:
      // - replace req URI with the URI in Route header,
      // - remove the Route header,
      // - proceed as if it received this modified request.
      msg->line.req.uri = hroute->name_addr.uri;
      req_uri = (pjsip_sip_uri*)msg->line.req.uri;
      pj_list_erase(hroute);
    }
  }

  // maddr handling for source routing is considered deprecated, so we don't
  // support it.  (See RFC 3261/19.1.1 - recommendation is to use Route headers
  // if requests must traverse a fixed set of proxies.)

  // Route on the top route header if present.
  hroute = (pjsip_route_hdr*)pjsip_msg_find_hdr(msg, PJSIP_H_ROUTE, NULL);
  if (hroute != NULL)
  {
    if ((!PJUtils::is_uri_local(hroute->name_addr.uri)) &&
        (!PJUtils::is_home_domain(hroute->name_addr.uri)))
    {
      // The top route header is not this node or the local domain so set up
      // a target containing just the Request URI so the requesst will be
      // routed to the next node in the route set.
      LOG_DEBUG("Route to next hop in route set");
      Target* target = new Target;
      target->uri = (pjsip_uri*)req_uri;
      add_target(target);
    }
    else
    {
      // The top route header indicates this proxy or home domain, so
      // MUST remove that value from the request.
      LOG_DEBUG("Remove top Route header referencing this node/domain");
      pj_list_erase(hroute);
    }
  }

  return PJSIP_SC_OK;
}


/// Create a PJSIP UAS transaction for handling stateful request proxying.
pj_status_t BasicProxy::UASTsx::create_pjsip_transaction(pjsip_rx_data* rdata)
{
  // Create a group lock, and take it.  This avoids the transaction being
  // destroyed before we even get our hands on it.  It is okay to use our
  // global pool here as PJSIP creates its own pool for the lock, using the
  // same factory as the supplied pool.
  pj_status_t status = pj_grp_lock_create(stack_data.pool, NULL, &_lock);
  if (status != PJ_SUCCESS)
  {
    // LCOV_EXCL_START
    LOG_DEBUG("Failed to create group lock for transaction");
    return status;
    // LCOV_EXCL_STOP
  }
  pj_grp_lock_add_ref(_lock);
  pj_grp_lock_acquire(_lock);

  // Create a transaction for the UAS side.  We do this before looking
  // up targets because calculating targets may involve interacting
  // with an external database, and we need the transaction in place
  // early to ensure CANCEL gets handled correctly.
  status = pjsip_tsx_create_uas2(_proxy->_mod_tu.module(),
                                 rdata,
                                 _lock,
                                 &_tsx);
  if (status != PJ_SUCCESS)
  {
    // LCOV_EXCL_START
    pj_grp_lock_release(_lock);
    pj_grp_lock_dec_ref(_lock);
    _lock = NULL;
    return status;
    // LCOV_EXCL_STOP
  }

  // Bind this object to the PJSIP transaction.
  _proxy->bind_transaction(this, _tsx);

  // Enter the transaction's context, and then release our copy of the
  // group lock, but don't decrement the reference count as we need to leave
  // a reference corresponding to this UASTsx structure.
  enter_context();
  pj_grp_lock_release(_lock);

  // Set the trail identifier for the transaction using the trail ID on
  // the original message.
  set_trail(_tsx, _trail);

  return PJ_SUCCESS;
}


/// Calculate a list of targets for the message.
int BasicProxy::UASTsx::calculate_targets()
{
  pjsip_msg* msg = _req->msg;

  // RFC 3261 Section 16.5 Determining Request Targets

  pjsip_sip_uri* req_uri = (pjsip_sip_uri*)msg->line.req.uri;

  // maddr handling is deprecated in favour of using Route headers to Route
  // requests, so is not supported.

  // If the domain of the Request-URI indicates a domain this element is
  // not responsible for, the Request-URI MUST be placed into the target
  // set as the only target, and the element MUST proceed to the task of
  // Request Forwarding (Section 16.6).
  if ((!PJUtils::is_home_domain((pjsip_uri*)req_uri)) &&
      (!PJUtils::is_uri_local((pjsip_uri*)req_uri)))
  {
    LOG_INFO("Route request to domain %.*s",
             req_uri->host.slen, req_uri->host.ptr);
    Target* target = new Target;
    add_target(target);
    return PJSIP_SC_OK;
  }

  return PJSIP_SC_NOT_FOUND;
}


/// Adds a target to the target list for this transaction.
void BasicProxy::UASTsx::add_target(BasicProxy::Target* target)
{
  _targets.push_back(target);
}


/// Initializes UAC transactions to each of the specified targets and
/// forwards the request.
///
/// @returns a status code indicating whether or not the operation succeeded.
pj_status_t BasicProxy::UASTsx::forward_to_targets()
{
  pj_status_t status = PJ_EUNKNOWN;

  if (_tsx != NULL)
  {
    // Initialise the UAC data structures for each new target.
    _pending_sends = _targets.size();

    while (!_targets.empty())
    {
      LOG_DEBUG("Allocating transaction and data for target");
      pjsip_tx_data* uac_tdata = PJUtils::clone_tdata(_req);

      if (uac_tdata == NULL)
      {
        // LCOV_EXCL_START
        status = PJ_ENOMEM;
        LOG_ERROR("Failed to clone request for forked transaction, %s",
                  PJUtils::pj_status_to_string(status).c_str());
        break;
        // LCOV_EXCL_STOP
      }

      // Set the target information in the request.
      Target* target = _targets.front();
      _targets.pop_front();
      set_req_target(uac_tdata, target); 
      delete target;

      // Forward the request.
      int index;
      --_pending_sends;
      ++_pending_responses;
      status = forward_request(uac_tdata, index);
      if (status != PJ_SUCCESS) 
      {
        // @TODO - handle errors better!!
        // LCOV_EXCL_START
        break;
        // LCOV_EXCL_STOP
      }
    }
  }

  return status;
}


/// Set the target for this request.
void BasicProxy::UASTsx::set_req_target(pjsip_tx_data* tdata,
                                        BasicProxy::Target* target)
{
  LOG_DEBUG("Set target for request");

  if (target->uri != NULL)
  {
    // Target has a URI, so write this in to the request URI in the request.
    // Need to clone the URI to make sure it comes from the right pool.
    LOG_DEBUG("Update Request-URI to %s",
              PJUtils::uri_to_string(PJSIP_URI_IN_REQ_URI, target->uri).c_str());
    tdata->msg->line.req.uri =
                        (pjsip_uri*)pjsip_uri_clone(tdata->pool, target->uri);
  }

  for (std::list<pjsip_uri*>::const_iterator pit = target->paths.begin();
       pit != target->paths.end();
       ++pit)
  {
    // We've got a path that should be added as a Route header.
    LOG_DEBUG("Adding a Route header to sip:%.*s%s%.*s",
              ((pjsip_sip_uri*)*pit)->user.slen, ((pjsip_sip_uri*)*pit)->user.ptr,
              (((pjsip_sip_uri*)*pit)->user.slen != 0) ? "@" : "",
              ((pjsip_sip_uri*)*pit)->host.slen, ((pjsip_sip_uri*)*pit)->host.ptr);
    pjsip_route_hdr* route_hdr = pjsip_route_hdr_create(tdata->pool);
    route_hdr->name_addr.uri = (pjsip_uri*)pjsip_uri_clone(tdata->pool, *pit);
    pjsip_msg_add_hdr(tdata->msg, (pjsip_hdr*)route_hdr);
  }

  if (target->transport != NULL)
  {
    // The target includes a selected transport, so set the transport on
    // the request.
    LOG_DEBUG("Force request to use selected transport %.*s:%d to %.*s:%d",
              target->transport->local_name.host.slen,
              target->transport->local_name.host.ptr,
              target->transport->local_name.port,
              target->transport->remote_name.host.slen,
              target->transport->remote_name.host.ptr,
              target->transport->remote_name.port);
    pjsip_tpselector tp_selector;
    tp_selector.type = PJSIP_TPSELECTOR_TRANSPORT;
    tp_selector.u.transport = target->transport;
    pjsip_tx_data_set_transport(tdata, &tp_selector);

    tdata->dest_info.addr.count = 1;
    tdata->dest_info.addr.entry[0].type = (pjsip_transport_type_e)target->transport->key.type;
    pj_memcpy(&tdata->dest_info.addr.entry[0].addr, &target->transport->key.rem_addr, sizeof(pj_sockaddr));
    tdata->dest_info.addr.entry[0].addr_len =
         (tdata->dest_info.addr.entry[0].addr.addr.sa_family == pj_AF_INET()) ?
         sizeof(pj_sockaddr_in) : sizeof(pj_sockaddr_in6);
    tdata->dest_info.cur_addr = 0;

    // Remove the reference to the transport added when it was chosen.
    pjsip_transport_dec_ref(target->transport);
  }
}


/// Forwards a request creating a UACTsx to handle the downstream hop.
pj_status_t BasicProxy::UASTsx::forward_request(pjsip_tx_data* tdata, int& index)
{
  index = _uac_tsx.size();

  // Create and initialize the UAC transaction.
  UACTsx* uac_tsx = create_uac_tsx(index);
  pj_status_t status = (uac_tsx != NULL) ? uac_tsx->init(tdata) : PJ_ENOMEM;

  if (status != PJ_SUCCESS)
  {
    // LCOV_EXCL_START
    LOG_ERROR("Failed to create/initialize UAC transaction, %s",
              PJUtils::pj_status_to_string(status).c_str());
    delete uac_tsx;
    // LCOV_EXCL_STOP
  }
  else
  {
    // Add the UAC transaction to the list.
    _uac_tsx.push_back(uac_tsx);

    // All the data structures, transactions and transmit data have
    // been created, so start send the request.
    uac_tsx->send_request();
  }
  return status;
}


/// Handles a response to an associated UACTsx.
void BasicProxy::UASTsx::on_new_client_response(UACTsx* uac_tsx,
                                                pjsip_tx_data *tdata)
{
  if (_tsx != NULL)
  {
    enter_context();

    int status_code = tdata->msg->line.status.code;

    if ((status_code == 100) &&
        (!_proxy->_delay_trying))
    {
      // Delay trying is disabled, so we will already have sent a locally
      // generated 100 Trying response, so don't forward this one.
      LOG_DEBUG("%s - Discard 100/INVITE response", uac_tsx->name());
      pjsip_tx_data_dec_ref(tdata);
      exit_context();
      return;
    }

    if ((status_code > 100) &&
        (status_code < 199) &&
        (_tsx->method.id == PJSIP_INVITE_METHOD))
    {
      // Forward all provisional responses to INVITEs.
      LOG_DEBUG("%s - Forward 1xx response", uac_tsx->name());

      // Forward response with the UAS transaction
      on_tx_response(tdata);
      pjsip_tsx_send_msg(_tsx, tdata);
    }
    else if (status_code == 200)
    {
      // 200 OK.
      LOG_DEBUG("%s - Forward 200 OK response", name());

      // Send this response immediately as a final response.
      if (_best_rsp != NULL)
      {
        pjsip_tx_data_dec_ref(_best_rsp);
      }
      _best_rsp = tdata;
      --_pending_responses;
      dissociate(uac_tsx);
      on_final_response();
    }
    else
    {
      // Final, non-OK response.  Is this the "best" response
      // received so far?
      LOG_DEBUG("%s - 3xx/4xx/5xx/6xx response", uac_tsx->name());
      if ((_best_rsp == NULL) ||
          (compare_sip_sc(status_code, _best_rsp->msg->line.status.code) > 0))
      {
        LOG_DEBUG("%s - Best 3xx/4xx/5xx/6xx response so far", uac_tsx->name());

        if (_best_rsp != NULL)
        {
          pjsip_tx_data_dec_ref(_best_rsp);
        }

        _best_rsp = tdata;
      }
      else
      {
        pjsip_tx_data_dec_ref(tdata);
      }

      // Disconnect the UAC data from the UAS data so no further
      // events get passed between the two.
      dissociate(uac_tsx);

      --_pending_responses;

      if (_pending_sends + _pending_responses == 0)
      {
        // Received responses on every UAC transaction, so check terminating
        // call services and then send the best response on the UAS
        // transaction.
        LOG_DEBUG("%s - All UAC responded", name());
        on_final_response();
      }
    }

    exit_context();
  }
// LCOV_EXCL_START
  else
  {
    pjsip_tx_data_dec_ref(tdata);
  }
// LCOV_EXCL_STOP
}


/// Notification that the underlying PJSIP transaction has changed state.
///
/// After calling this, the caller must not assume that the UASTsx still
/// exists - if the PJSIP transaction is being destroyed, this method will
/// destroy the UASTsx.
void BasicProxy::UASTsx::on_tsx_state(pjsip_event* event)
{
  enter_context();

  if (_tsx->state == PJSIP_TSX_STATE_COMPLETED)
  {
    // UAS transaction has completed, so do any transaction completion
    // activities.
    on_tsx_complete();
  }

  if (_tsx->state == PJSIP_TSX_STATE_DESTROYED)
  {
    LOG_DEBUG("%s - UAS tsx destroyed", _tsx->obj_name);
    if (_tsx->method.id == PJSIP_INVITE_METHOD)
    {
      // INVITE transaction has been terminated.  If there are any
      // pending UAC transactions they should be cancelled.
      cancel_pending_uac_tsx(0, true);
    }
    _proxy->unbind_transaction(_tsx);
    _tsx = NULL;
    _pending_destroy = true;
  }

  exit_context();
}


/// Handles the best final response, once all final responses have been received
/// from all forked INVITEs.
void BasicProxy::UASTsx::on_final_response()
{
  if (_tsx != NULL)
  {
    pjsip_tx_data *best_rsp = _best_rsp;
    int st_code = best_rsp->msg->line.status.code;
    _best_rsp = NULL;
    set_trail(best_rsp, trail());
    on_tx_response(best_rsp);
    pjsip_tsx_send_msg(_tsx, best_rsp);

    if ((_tsx->method.id == PJSIP_INVITE_METHOD) &&
        (st_code == 200))
    {
      // Terminate the UAS transaction (this needs to be done
      // manually for INVITE 200 OK response, otherwise the
      // transaction layer will wait for an ACK).  This will also
      // cause all other pending UAC transactions to be cancelled.
      LOG_DEBUG("%s - Terminate UAS INVITE transaction", _tsx->obj_name);
      pjsip_tsx_terminate(_tsx, 200);
    }
  }
}


/// Sends a response using the buffer saved off for the best response.
void BasicProxy::UASTsx::send_response(int st_code, const pj_str_t* st_text)
{
  if ((st_code >= 100) && (st_code < 200))
  {
    // Send a provisional response.
    // LCOV_EXCL_START
    pjsip_tx_data* prov_rsp = PJUtils::clone_tdata(_best_rsp);
    prov_rsp->msg->line.status.code = st_code;
    prov_rsp->msg->line.status.reason =
                (st_text != NULL) ? *st_text : *pjsip_get_status_text(st_code);
    pjsip_tx_data_invalidate_msg(prov_rsp);
    set_trail(prov_rsp, trail());
    on_tx_response(prov_rsp);
    pjsip_tsx_send_msg(_tsx, prov_rsp);
    // LCOV_EXCL_STOP
  }
  else
  {
    // Send a final response.
    pjsip_tx_data *best_rsp = _best_rsp;
    _best_rsp = NULL;
    best_rsp->msg->line.status.code = st_code;
    best_rsp->msg->line.status.reason =
                (st_text != NULL) ? *st_text : *pjsip_get_status_text(st_code);
    pjsip_tx_data_invalidate_msg(best_rsp);
    set_trail(best_rsp, trail());
    on_tx_response(best_rsp);
    pjsip_tsx_send_msg(_tsx, best_rsp);
  }
}


/// Called when a response is transmitted on this transaction.
void BasicProxy::UASTsx::on_tx_response(pjsip_tx_data* tdata)
{
}


/// Called when a request is transmitted on an associated downstream client
/// transaction.
void BasicProxy::UASTsx::on_tx_client_request(pjsip_tx_data* tdata, UACTsx* uac_tsx)
{
}


/// Perform actions on a new transaction starting.
void BasicProxy::UASTsx::on_tsx_start(const pjsip_rx_data* rdata)
{
  // Report SAS markers for the transaction.
  LOG_DEBUG("Report SAS start marker - trail (%llx)", trail());
  SAS::Marker start_marker(trail(), MARKER_ID_START, 1u);
  SAS::report_marker(start_marker);

  if (rdata->msg_info.from != NULL)
  {
    SAS::Marker calling_dn(trail(), MARKER_ID_CALLING_DN, 1u);
    pjsip_sip_uri* calling_uri = (pjsip_sip_uri*)pjsip_uri_get_uri(rdata->msg_info.from->uri);
    calling_dn.add_var_param(calling_uri->user.slen, calling_uri->user.ptr);
    SAS::report_marker(calling_dn);
  }

  if (rdata->msg_info.to != NULL)
  {
    SAS::Marker called_dn(trail(), MARKER_ID_CALLED_DN, 1u);
    pjsip_sip_uri* called_uri = (pjsip_sip_uri*)pjsip_uri_get_uri(rdata->msg_info.to->uri);
    called_dn.add_var_param(called_uri->user.slen, called_uri->user.ptr);
    SAS::report_marker(called_dn);
  }

  PJUtils::mark_sas_call_branch_ids(trail(), rdata->msg_info.cid, rdata->msg_info.msg);
}


/// Perform actions on a transaction completing.
void BasicProxy::UASTsx::on_tsx_complete()
{
  // Report SAS markers for the transaction.
  LOG_DEBUG("Report SAS end marker - trail (%llx)", trail());
  SAS::Marker end_marker(trail(), MARKER_ID_END, 1u);
  SAS::report_marker(end_marker);
}


/// Cancels all pending UAC transactions associated with this UAS transaction.
void BasicProxy::UASTsx::cancel_pending_uac_tsx(int st_code, bool dissociate_uac)
{
  enter_context();

  // Send CANCEL on all pending UAC transactions forked from this UAS
  // transaction.  This is invoked either because the UAS transaction
  // received a CANCEL, or one of the UAC transactions received a 200 OK or
  // 6xx response.
  UACTsx *uac_tsx;

  LOG_DEBUG("%s - Cancel %d pending UAC transactions",
            name(), _pending_responses);

  for (size_t ii = 0; ii < _uac_tsx.size(); ++ii)
  {
    uac_tsx = _uac_tsx[ii];
    LOG_DEBUG("%s - Check target %d, UAC data = %p, UAC tsx = %p",
              name(),
              ii,
              uac_tsx,
              (uac_tsx != NULL) ? uac_tsx->_tsx : NULL);

    if (uac_tsx != NULL)
    {
      // Found a UAC transaction that is still active, so send a CANCEL.
      uac_tsx->cancel_pending_tsx(st_code);

      // Normal behaviour (that is, on receipt of a CANCEL on the UAS
      // transaction), is to leave the UAC transaction connected to the UAS
      // transaction so the 487 response gets passed through.  However, in
      // cases where the CANCEL is initiated on this node (for example,
      // because the UAS transaction has already failed, or in call forwarding
      // scenarios) we dissociate immediately so the 487 response gets
      // swallowed on this node
      if (dissociate_uac)
      {
        dissociate(uac_tsx);
      }
    }
  }

  exit_context();
}


/// Compare two status codes from the perspective of which is the best to
/// return to the originator of a forked transaction.  This will only ever
/// be called for 3xx/4xx/5xx/6xx response codes.
///
/// @returns +1 if sc1 is better than sc2
///          0 if sc1 and sc2 are identical (or equally as good)
///          -1 if sc2 is better than sc1
///
int BasicProxy::UASTsx::compare_sip_sc(int sc1, int sc2)
{
  // Order is: (best) 487, 300, 301, ..., 698, 699, 408 (worst).
  LOG_DEBUG("Compare new status code %d with stored status code %d", sc1, sc2);
  if (sc1 == sc2)
  {
    // Status codes are equal.
    return 0;
  }
  else if (sc1 == PJSIP_SC_REQUEST_TIMEOUT)
  {
    // A timeout response is never better than anything else.
    return -1;
  }
  else if (sc2 == PJSIP_SC_REQUEST_TIMEOUT)
  {
    // A non-timeout response is always better than a timeout.
    return 1;
  }
  else if (sc2 == PJSIP_SC_REQUEST_TERMINATED)
  {
    // Request terminated is always better than anything else because
    // this should only happen if transaction is CANCELLED by originator
    // and this will be the expected response.
    return -1;
  }
  else if (sc1 == PJSIP_SC_REQUEST_TERMINATED)
  {
    return 1;
  }
  // Default behaviour is to favour the lowest number.
  else if (sc1 < sc2)
  {
    return 1;
  }
  else
  {
    return -1;
  }
}


/// Disassociates the specified UAC transaction from this UAS transaction, and
/// vice-versa.  Must be called before destroying either transaction.
void BasicProxy::UASTsx::dissociate(UACTsx* uac_tsx)
{
  LOG_DEBUG("Dissociate UAC transaction %p for target %d", uac_tsx, uac_tsx->_index);
  uac_tsx->_uas_tsx = NULL;
  _uac_tsx[uac_tsx->_index] = NULL;
}


/// Creates a UACTsx object to send the request to a selected target.
BasicProxy::UACTsx* BasicProxy::UASTsx::create_uac_tsx(size_t index)
{
  return new UACTsx(_proxy, this, index);
}


/// Enters this transaction's context.  While in the transaction's
/// context, it will not be destroyed.  Whenever enter_context is called,
/// exit_context must be called before the end of the method.
void BasicProxy::UASTsx::enter_context()
{
  if (_lock != NULL)
  {
    // Take the group lock.
    pj_grp_lock_acquire(_lock);
  }

  // If the transaction is pending destroy, the context count must be greater
  // than 0.  Otherwise, the transaction should have already been destroyed (so
  // entering its context again is unsafe).
  pj_assert((!_pending_destroy) || (_context_count > 0));

  _context_count++;
}


/// Exits this transaction's context.  On return from this method, the caller
/// must not assume that the transaction still exists.
void BasicProxy::UASTsx::exit_context()
{
  // If the transaction is pending destroy, the context count must be greater
  // than 0.  Otherwise, the transaction should have already been destroyed (so
  // entering its context again is unsafe).
  pj_assert(_context_count > 0);

  _context_count--;
  if ((_context_count == 0) && (_pending_destroy))
  {
    delete this;
  }
  else if (_lock != NULL)
  {
    // Release the group lock.
    pj_grp_lock_release(_lock);
  }
}

void BasicProxy::UASTsx::cancel_trying_timer()
{
  pthread_mutex_lock(&_trying_timer_lock);

  if (_trying_timer.id == TRYING_TIMER)
  {
    // The deferred trying timer is running, so cancel it.
    _trying_timer.id = 0;
    pjsip_endpt_cancel_timer(stack_data.endpt, &_trying_timer);
  }

  pthread_mutex_unlock(&_trying_timer_lock);
}

/// Handle the trying timer expiring on this transaction.
void BasicProxy::UASTsx::trying_timer_expired()
{
  enter_context();
  pthread_mutex_lock(&_trying_timer_lock);

  if ((_trying_timer.id == TRYING_TIMER) &&
      (_tsx != NULL) &&
      (_tsx->state == PJSIP_TSX_STATE_TRYING))
  {
    // Transaction is still in Trying state, so send a 100 Trying response
    // now.
    send_response(100);
    _trying_timer.id = 0;
  }

  pthread_mutex_unlock(&_trying_timer_lock);
  exit_context();
}

/// Static method called by PJSIP when a trying timer expires.  The instance
/// is stored in the user_data field of the timer entry.
void BasicProxy::UASTsx::trying_timer_callback(pj_timer_heap_t *timer_heap, struct pj_timer_entry *entry)
{
  if (entry->id == TRYING_TIMER)
  {
    ((BasicProxy::UASTsx*)entry->user_data)->trying_timer_expired();
  }
}

/// UACTsx constructor
BasicProxy::UACTsx::UACTsx(BasicProxy* proxy,
                           UASTsx* uas_tsx,
                           size_t index) :
  _proxy(proxy),
  _uas_tsx(uas_tsx),
  _index(index),
  _tdata(NULL),
  _servers(),
  _current_server(0),
  _pending_destroy(false),
  _context_count(0)
{
  // Don't put any initialization that can fail here, implement in init()
  // instead.
  _lock = _uas_tsx->_lock;
  pj_grp_lock_add_ref(_lock);
}

/// UACTsx destructor
BasicProxy::UACTsx::~UACTsx()
{
  LOG_DEBUG("BasicProxy::UACTsx destructor (%p)", this);
  pj_assert(_context_count == 0);

  if (_tsx != NULL)
  {
    _proxy->unbind_transaction(_tsx);                                   //LCOV_EXCL_LINE
  }

  if (_uas_tsx != NULL)
  {
    _uas_tsx->dissociate(this);                                         //LCOV_EXCL_LINE
  }

  if (_tdata != NULL)
  {
    pjsip_tx_data_dec_ref(_tdata);                                      //LCOV_EXCL_LINE
    _tdata = NULL;                                                      //LCOV_EXCL_LINE
  }

  if ((_tsx != NULL) &&
      (_tsx->state != PJSIP_TSX_STATE_TERMINATED) &&
      (_tsx->state != PJSIP_TSX_STATE_DESTROYED))
  {
    pjsip_tsx_terminate(_tsx, PJSIP_SC_INTERNAL_SERVER_ERROR);          //LCOV_EXCL_LINE
  }

  _tsx = NULL;

  pj_grp_lock_release(_lock);
  pj_grp_lock_dec_ref(_lock);
}


/// Initializes a UAC transaction.
pj_status_t BasicProxy::UACTsx::init(pjsip_tx_data* tdata)
{
  pj_status_t status;

  // Store the request and add a reference to the request so we can be sure it
  // remains valid for retries.
  _tdata = tdata;
  pjsip_tx_data_add_ref(_tdata);

  // Create a PJSIP UAC transaction for the request and bind it to this
  // object.
  status = pjsip_tsx_create_uac2(_proxy->_mod_tu.module(),
                                 tdata,
                                 _lock,
                                 &_tsx);
  if (status != PJ_SUCCESS)
  {
    LOG_DEBUG("Failed to create PJSIP UAC transaction");                //LCOV_EXCL_LINE
    return status;                                                      //LCOV_EXCL_LINE
  }

  // Set up the PJSIP transaction user module data to refer to the associated
  // UACTsx object
  _proxy->bind_transaction(this, _tsx);

  // Add the trail from the UAS transaction to the UAC transaction.
  set_trail(_tsx, _uas_tsx->trail());
  LOG_DEBUG("Added trail identifier %ld to UAC transaction", get_trail(_tsx));

  if (_tdata->tp_sel.type != PJSIP_TPSELECTOR_TRANSPORT)
  {
    // Resolve the next hop destination for this request to a set of target
    // servers (IP address/port/transport tuples).
    PJUtils::resolve_next_hop(_tdata, 0, _servers, trail());
  }

  return PJ_SUCCESS;
}


/// Sends the initial request on this UAC transaction.
void BasicProxy::UACTsx::send_request()
{
  enter_context();

  pj_status_t status = PJ_SUCCESS;

  LOG_DEBUG("Sending request for %s",
            PJUtils::uri_to_string(PJSIP_URI_IN_REQ_URI, _tdata->msg->line.req.uri).c_str());

  if (_tdata->tp_sel.type == PJSIP_TPSELECTOR_TRANSPORT)
  {
    // The transport has already been selected for this request, so
    // add it to the transaction otherwise it will get overwritten.
    LOG_DEBUG("Transport %s (%s) pre-selected for transaction",
              _tdata->tp_sel.u.transport->obj_name,
              _tdata->tp_sel.u.transport->info);
    pjsip_tsx_set_transport(_tsx, &_tdata->tp_sel);
  }
  else if (_current_server < (int)_servers.size())
  {
    // We have resolved servers to try, so set up the destination information
    // in the request.
    PJUtils::set_dest_info(_tdata, _servers[_current_server]);
  }
  else
  {
    // We failed to get any valid destination servers, so fail the transaction.
    status = PJ_ENOTFOUND;
  }

  if (status == PJ_SUCCESS)
  {
    // Notify the UASTsx the request is being sent and send it.
    _uas_tsx->on_tx_client_request(_tdata, this);
    status = pjsip_tsx_send_msg(_tsx, _tdata);
  }

  if (status != PJ_SUCCESS)
  {
    // Failed to send the request.  This is an unexpected error rather than
    // an indication that the selected destination is down, so we do not
    // attempt a retry and do not blacklist the selected destination.
    LOG_DEBUG("Failed to send request (%d %s)",                         //LCOV_EXCL_LINE
              status, PJUtils::pj_status_to_string(status).c_str());    //LCOV_EXCL_LINE
    pjsip_tx_data_dec_ref(_tdata);                                      //LCOV_EXCL_LINE

    // The UAC transaction will have been destroyed when it failed to send
    // the request, so there's no need to destroy it.  However, we do need to
    // tell the UAS transaction.
    if (_uas_tsx != NULL)                                               //LCOV_EXCL_LINE
    {                                                                   //LCOV_EXCL_LINE
      send_timeout_response();                                          //LCOV_EXCL_LINE
    }                                                                   //LCOV_EXCL_LINE
  }

  exit_context();
}


/// Cancels the pending transaction, using the specified status code in the
/// Reason header.
void BasicProxy::UACTsx::cancel_pending_tsx(int st_code)
{
  if (_tsx != NULL)
  {
    enter_context();

    LOG_DEBUG("Found transaction %s status=%d", name(), _tsx->status_code);
    if (_tsx->status_code < 200)
    {
      pjsip_tx_data *cancel;
      pjsip_endpt_create_cancel(stack_data.endpt, _tsx->last_tx, &cancel);
      if (st_code != 0)
      {
        char reason_val_str[96];                                        //LCOV_EXCL_LINE
        const pj_str_t* st_text = pjsip_get_status_text(st_code);       //LCOV_EXCL_LINE
        sprintf(reason_val_str,                                         //LCOV_EXCL_LINE
                "SIP ;cause=%d ;text=\"%.*s\"",                         //LCOV_EXCL_LINE
                st_code,                                                //LCOV_EXCL_LINE
                (int)st_text->slen,                                     //LCOV_EXCL_LINE
                st_text->ptr);                                          //LCOV_EXCL_LINE
        pj_str_t reason_name = pj_str("Reason");                        //LCOV_EXCL_LINE
        pj_str_t reason_val = pj_str(reason_val_str);                   //LCOV_EXCL_LINE
        pjsip_hdr* reason_hdr =                                         //LCOV_EXCL_LINE
               (pjsip_hdr*)pjsip_generic_string_hdr_create(cancel->pool,//LCOV_EXCL_LINE
                                                           &reason_name,//LCOV_EXCL_LINE
                                                           &reason_val);//LCOV_EXCL_LINE
        pjsip_msg_add_hdr(cancel->msg, reason_hdr);                     //LCOV_EXCL_LINE
      }
      if (get_trail(_tsx) == 0)
      {
        LOG_ERROR("Sending CANCEL request with no SAS trail");
      }
      set_trail(cancel, get_trail(_tsx));

      if (_tsx->transport != NULL)
      {
        // The transaction being cancelled has already selected a transport,
        // so make sure the CANCEL uses this transport as well.
        pjsip_tpselector tp_selector;
        tp_selector.type = PJSIP_TPSELECTOR_TRANSPORT;
        tp_selector.u.transport = _tsx->transport;
        pjsip_tx_data_set_transport(cancel, &tp_selector);
      }

      LOG_DEBUG("Sending CANCEL request");
      pj_status_t status = pjsip_endpt_send_request(stack_data.endpt, cancel, -1, NULL, NULL);
      if (status != PJ_SUCCESS)
      {
        LOG_ERROR("Error sending CANCEL, %s",                           //LCOV_EXCL_LINE
                  PJUtils::pj_status_to_string(status).c_str());        //LCOV_EXCL_LINE
      }
    }

    exit_context();
  }
}


/// Notification that the underlying PJSIP transaction has changed state.
///
/// After calling this, the caller must not assume that the UACTsx still
/// exists - if the PJSIP transaction is being destroyed, this method will
/// destroy the UACTsx.
void BasicProxy::UACTsx::on_tsx_state(pjsip_event* event)
{
  enter_context();

  // Handle incoming responses (provided the UAS transaction hasn't
  // terminated or been cancelled).
  LOG_DEBUG("%s - uac_tsx = %p, uas_tsx = %p", name(), this, _uas_tsx);

  // Check that the event is on the current UAC transaction (we may have
  // created a new one for a retry) and is still connected to the UAS
  // transaction.
  if ((event->body.tsx_state.tsx == _tsx) && (_uas_tsx != NULL))
  {
    bool retrying = false;

    if (!_servers.empty())
    {
      // Check to see if the destination server has failed so we can blacklist
      // it and retry to an alternative if possible.
      if ((_tsx->state == PJSIP_TSX_STATE_TERMINATED) &&
          ((event->body.tsx_state.type == PJSIP_EVENT_TIMER) ||
           (event->body.tsx_state.type == PJSIP_EVENT_TRANSPORT_ERROR)))
      {
        // Either failed to connect to the selected server, or failed or get
        // a response, so blacklist it.
        LOG_DEBUG("Failed to connected to server or timed-out, so add to blacklist");
        PJUtils::blacklist_server(_servers[_current_server]);

        // Attempt a retry.
        retrying = retry_request();
      }
      else if ((_tsx->state == PJSIP_TSX_STATE_COMPLETED) &&
               (PJSIP_IS_STATUS_IN_CLASS(_tsx->status_code, 500)))
      {
        // The server returned a 5xx error.  We don't blacklist in this case
        // as it may indicated a transient overload condition, but we can
        // retry to an alternate server if one is available.
        LOG_DEBUG("Server return 5xx error");
        retrying = retry_request();
      }
    }

    if (!retrying)
    {
      if (event->body.tsx_state.type == PJSIP_EVENT_RX_MSG)
      {
        LOG_DEBUG("%s - RX_MSG on active UAC transaction", name());
        if (_uas_tsx != NULL)
        {
          pjsip_tx_data* tdata;
          pj_status_t status = PJUtils::create_response_fwd(stack_data.endpt,
                                                            event->body.tsx_state.src.rdata,
                                                            0,
                                                            &tdata);
          if (status != PJ_SUCCESS)
          {
            // LCOV_EXCL_START
            LOG_ERROR("Error creating response, %s",
                      PJUtils::pj_status_to_string(status).c_str());
            // LCOV_EXCL_STOP
          }
          else
          {
            _uas_tsx->on_new_client_response(this, tdata);
          }
        }
      }

      // If UAC transaction is terminated because of a timeout, treat this as
      // a 504 error.
      if ((_tsx->state == PJSIP_TSX_STATE_TERMINATED) &&
          (_uas_tsx != NULL))
      {
        // UAC transaction has terminated while still connected to the UAS
        // transaction.
        LOG_DEBUG("%s - UAC tsx terminated while still connected to UAS tsx",
                  _tsx->obj_name);
        if ((event->body.tsx_state.type == PJSIP_EVENT_TIMER) ||
            (event->body.tsx_state.type == PJSIP_EVENT_TRANSPORT_ERROR))
        {
          LOG_DEBUG("Timeout or transport error");
          send_timeout_response();
        }
      }
    }
  }

  if ((event->body.tsx_state.tsx == _tsx) &&
      (_tsx->state == PJSIP_TSX_STATE_DESTROYED))
  {
    LOG_DEBUG("%s - UAC tsx destroyed", _tsx->obj_name);
    _proxy->unbind_transaction(_tsx);
    _tsx = NULL;
    _pending_destroy = true;
  }

  exit_context();
}


/// Builds and sends a timeout response upstream for this UAC transaction.
void BasicProxy::UACTsx::send_timeout_response()
{
  pjsip_tx_data* tdata;
  pj_status_t status = PJUtils::create_response(stack_data.endpt,
                                                _tdata,
                                                PJSIP_SC_REQUEST_TIMEOUT,
                                                NULL,
                                                &tdata);
  if (status != PJ_SUCCESS)
  {
    // LCOV_EXCL_START
    LOG_ERROR("Error creating response, %s",
              PJUtils::pj_status_to_string(status).c_str());
    // LCOV_EXCL_STOP
  }
  else
  {
    _uas_tsx->on_new_client_response(this, tdata);
  }
}


/// Attempt to retry the request to an alternate server.
bool BasicProxy::UACTsx::retry_request()
{
  bool retrying = false;

  // See if we have any more servers in the list returned by the resolver that
  // we can try.
  _current_server++;
  if (_current_server < (int)_servers.size())
  {
    // More servers to try.  As per RFC3263, retries to an alternate server
    // have to be a completely new transaction, presumably to avoid any
    // possibility of mis-correlating a late response from the original server.
    // We therefore have to allocate a new branch ID and transaction for the
    // retry and connect it to this object.  We'll leave the old transaction
    // connected to this object while PJSIP closes it down, but ignore any
    // future events from it.
    LOG_DEBUG("Attempt to retry request to alternate server");
    pjsip_transaction* retry_tsx;
    PJUtils::generate_new_branch_id(_tdata);
    pj_status_t status = pjsip_tsx_create_uac2(_proxy->_mod_tu.module(),
                                               _tdata,
                                               _lock,
                                               &retry_tsx);

    if (status == PJ_SUCCESS)
    {
      // Set up the PJSIP transaction user module data to refer to the associated
      // UACTsx object
      LOG_DEBUG("Created transaction for retry, so send request");
      _proxy->unbind_transaction(_tsx);
      pjsip_transaction* original_tsx = _tsx;
      _tsx = retry_tsx;
      _proxy->bind_transaction(this, _tsx);

      // Add the trail from the UAS transaction to the UAC transaction.
      set_trail(_tsx, _uas_tsx->trail());

      // Increment the reference count of the request as we are passing
      // it to a new transaction.
      pjsip_tx_data_add_ref(_tdata);

      // Copy across the destination information for a retry and try to
      // resend the request.
      PJUtils::set_dest_info(_tdata, _servers[_current_server]);
      status = pjsip_tsx_send_msg(_tsx, _tdata);

      if (status == PJ_SUCCESS)
      {
        // Successfully sent the retry.
        LOG_INFO("Retrying request to alternate target");
        retrying = true;
      }
      else
      {
        // Failed to send, so revert to the original transaction to see it
        // through to the end.  Must decrement the reference count on the
        // request as pjsip_tsx_send_msg won't do it if it fails.
        // LCOV_EXCL_START
        LOG_INFO("Failed to send retry");
        pjsip_tx_data_dec_ref(_tdata);
        _proxy->unbind_transaction(_tsx);
        _tsx = original_tsx;
        _proxy->bind_transaction(this, _tsx);
        // LCOV_EXCL_STOP
      }
    }
  }

  return retrying;
}


/// Enters this transaction's context.  While in the transaction's
/// context, it will not be destroyed.  Whenever enter_context is called,
/// exit_context must be called before the end of the method.
void BasicProxy::UACTsx::enter_context()
{
  // Take the group lock.
  pj_grp_lock_acquire(_lock);

  // If the transaction is pending destroy, the context count must be greater
  // than 0.  Otherwise, the transaction should have already been destroyed (so
  // entering its context again is unsafe).
  pj_assert((!_pending_destroy) || (_context_count > 0));

  _context_count++;
}


/// Exits this transaction's context.  On return from this method, the caller
/// must not assume that the transaction still exists.
void BasicProxy::UACTsx::exit_context()
{
  // If the transaction is pending destroy, the context count must be greater
  // than 0.  Otherwise, the transaction should have already been destroyed (so
  // entering its context again is unsafe).
  pj_assert(_context_count > 0);

  _context_count--;
  if ((_context_count == 0) && (_pending_destroy))
  {
    delete this;
  }
  else
  {
    // Release the group lock.
    pj_grp_lock_release(_lock);
  }
}

