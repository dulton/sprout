/**
 * @file icscfproxy.h  I-CSCF proxy class definition
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

#ifndef ICSCFPROXY_H__
#define ICSCFPROXY_H__

#include "hssconnection.h"
#include "scscfselector.h"
#include "servercaps.h"
#include "acr.h"
#include "icscfrouter.h"
#include "basicproxy.h"

class ICSCFProxy : public BasicProxy
{
public:
  /// Constructor.
  ICSCFProxy(pjsip_endpoint* endpt,
             int port,
             int priority,
             HSSConnection* hss,
             ACRFactory* acr_factory,
             SCSCFSelector* scscf_selector);

  /// Destructor.
  ~ICSCFProxy();

protected:
  /// Process received requests not absorbed by transaction layer.
  virtual pj_bool_t on_rx_request(pjsip_rx_data* rdata);

  /// Utility to verify incoming requests.
  /// Return the SIP status code if verification failed.
  virtual int verify_request(pjsip_rx_data* rdata);

  /// Rejects a received request statelessly.
  virtual void reject_request(pjsip_rx_data* rdata, int status_code);

  /// Create I-CSCF UAS transaction objects.
  BasicProxy::UASTsx* create_uas_tsx();

private:

  class UASTsx : public BasicProxy::UASTsx
  {
  public:
    /// Constructor.
    UASTsx(BasicProxy* proxy);

    /// Destructor.
    ~UASTsx();

    /// Initialise the UAS transaction.
    virtual pj_status_t init(pjsip_rx_data* rdata);

    /// Handle a received CANCEL request.
    virtual void process_cancel_request(pjsip_rx_data* rdata);

  protected:
    /// Calculate targets for incoming requests by querying HSS.
    virtual int calculate_targets();

    /// Called when the final response has been determined.
    virtual void on_final_response();

  private:
    /// Handles a response to an associated UACTsx.
    virtual void on_new_client_response(UACTsx* uac_tsx,
                                        pjsip_tx_data *tdata);

    /// Notification that a response is being transmitted on this transaction.
    virtual void on_tx_response(pjsip_tx_data* tdata);

    /// Notification that a request is being transmitted to a client.
    virtual void on_tx_client_request(pjsip_tx_data* tdata,
                                      UACTsx* uac_tsx);

    /// Attempts to retry the request to an alternative S-CSCF.
    bool retry_to_alternate_scscf(int rsp_status);

    /// Create an ACR if ACR generation is enabled.
    ACR* create_acr(pjsip_rx_data* rdata);

    /// Defines the session case for the current transaction.
    typedef enum {REGISTER, ORIGINATING, TERMINATING} SessionCase;
    SessionCase _case;

    /// I-CSCF router object for the request.
    ICSCFRouter* _router;

    /// The ACR for the request (if ACR generation is enabled).
    ACR* _acr;

    /// Records whether or not the request is an in-dialog request.  This is
    /// used when deciding whether to send ACRs on provisional responses.
    bool _in_dialog;
  };

  /// Port for I-CSCF function.  This proxy will only process requests
  /// sent to this port, and leave other requests to other PJSIP modules.
  int _port;

  /// Homestead connection class for performing HSS queries.
  HSSConnection* _hss;

  /// S-CSCF selector used to select S-CSCFs from configuration.
  SCSCFSelector* _scscf_selector;

  /// ACR factory for I-CSCF ACRs.
  ACRFactory* _acr_factory;
};


#endif
