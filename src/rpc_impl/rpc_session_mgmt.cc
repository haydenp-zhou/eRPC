#include "rpc.h"
#include <algorithm>

namespace ERpc {

/**
 * @brief Process all session management events in the queue and free them.
 * The handlers for individual request/response types should not free packets.
 */
template <class Transport_>
void Rpc<Transport_>::handle_session_management() {
  assert(sm_hook.session_mgmt_ev_counter > 0);
  sm_hook.session_mgmt_mutex.lock();

  /* Handle all session management requests */
  for (SessionMgmtPkt *sm_pkt : sm_hook.session_mgmt_pkt_list) {
    /* The sender of a packet cannot be this Rpc */
    if (session_mgmt_is_pkt_type_req(sm_pkt->pkt_type)) {
      assert(!(strcmp(sm_pkt->client.hostname, nexus->hostname) == 0 &&
               sm_pkt->client.app_tid == app_tid));
    } else {
      assert(!(strcmp(sm_pkt->server.hostname, nexus->hostname) == 0 &&
               sm_pkt->server.app_tid == app_tid));
    }

    switch (sm_pkt->pkt_type) {
      case SessionMgmtPktType::kConnectReq:
        handle_session_connect_req(sm_pkt);
        break;
      case SessionMgmtPktType::kConnectResp:
        handle_session_connect_resp(sm_pkt);
        break;
      case SessionMgmtPktType::kDisconnectReq:
        handle_session_connect_resp(sm_pkt);
        break;
      case SessionMgmtPktType::kDisconnectResp:
        handle_session_connect_resp(sm_pkt);
        break;
      default:
        assert(false);
        break;
    }

    /* Free memory that was allocated by the Nexus */
    free(sm_pkt);
  }

  sm_hook.session_mgmt_pkt_list.clear();
  sm_hook.session_mgmt_ev_counter = 0;
  sm_hook.session_mgmt_mutex.unlock();
};

/**
 * @brief Handle a session connect request
 */
template <class Transport_>
void Rpc<Transport_>::handle_session_connect_req(SessionMgmtPkt *sm_pkt) {
  assert(sm_pkt != NULL);
  assert(sm_pkt->pkt_type == SessionMgmtPktType::kConnectReq);

  /* Ensure that server fields known by the client were filled correctly */
  assert(sm_pkt->server.app_tid == app_tid);
  assert(strcmp(sm_pkt->server.hostname, nexus->hostname) == 0);

  /* Create the basic issue message */
  char issue_msg[kMaxIssueMsgLen];
  sprintf(issue_msg, "eRPC Rpc: Rpc %s received connect request from %s. Issue",
          get_name().c_str(), sm_pkt->client.name().c_str());

  /* Check if the requested fabric port is managed by us */
  if (!is_fdev_port_managed(sm_pkt->server.fdev_port_index)) {
    erpc_dprintf("%s. Invalid server fabric port %zu.\n", issue_msg,
                 sm_pkt->server.fdev_port_index);

    sm_pkt->send_resp_mut(SessionMgmtErrType::kInvalidRemotePort,
                          &nexus->udp_config);
    return;
  }

  /* Check that the transport matches */
  if (sm_pkt->server.transport_type != transport->transport_type) {
    erpc_dprintf("%s: Invalid transport type %s.\n", issue_msg,
                 get_transport_name(sm_pkt->server.transport_type).c_str());

    sm_pkt->send_resp_mut(SessionMgmtErrType::kInvalidTransport,
                          &nexus->udp_config);
    return;
  }

  /*
   * Check if we (= this Rpc) already have a session as the server with the
   * client Rpc (C) that sent this packet. (This is different from if we have a
   * session as the client Rpc, where C is the server Rpc.) This happens when
   * the connect request is retransmitted.
   */
  for (Session *old_session : session_vec) {
    /*
     * This check ensures that we own the session as the server.
     *
     * If the check succeeds, we cannot own @old_session as the client:
     * @sm_pkt was sent by a different Rpc than us, since an Rpc cannot send
     * session management packets to itself. So the client hostname and app_tid
     * in the located session cannot be ours, since they are same as @sm_pkt's.
     */
    if ((old_session != nullptr) &&
        strcmp(old_session->client.hostname, sm_pkt->client.hostname) == 0 &&
        (old_session->client.app_tid == sm_pkt->client.app_tid)) {
      assert(old_session->role == Session::Role::kServer);
      assert(old_session->state == SessionState::kConnected);

      /* There's a valid session, so client's metadata cannot have changed. */
      assert(memcmp((void *)&old_session->client, (void *)&sm_pkt->client,
                    sizeof(old_session->client)) == 0);

      erpc_dprintf("%s: Duplicate session connect request.\n", issue_msg);

      /* Send a connect success response */
      sm_pkt->server = old_session->server; /* Fill in server metadata */
      sm_pkt->send_resp_mut(SessionMgmtErrType::kNoError, &nexus->udp_config);
      return;
    }
  }

  /* Check if we are allowed to create another session */
  if (session_vec.size() == kMaxSessionsPerThread) {
    erpc_dprintf("%s: Reached session limit %zu.\n", issue_msg,
                 kMaxSessionsPerThread);

    sm_pkt->send_resp_mut(SessionMgmtErrType::kTooManySessions,
                          &nexus->udp_config);
    return;
  }

  /* If we are here, it's OK to create a new session */
  Session *session =
      new Session(Session::Role::kServer, SessionState::kConnected);

  /* Set the server metadata fields in the packet */
  sm_pkt->server.session_num = (uint32_t)session_vec.size();
  sm_pkt->server.start_seq = generate_start_seq();
  transport->fill_routing_info(&(sm_pkt->server.routing_info));

  /* Copy the packet's metadata to the created session */
  session->server = sm_pkt->server;
  session->client = sm_pkt->client;

  sm_pkt->send_resp_mut(SessionMgmtErrType::kNoError, &nexus->udp_config);
  return;
}

/**
 * @brief Handle a session connect response.
 */
template <class Transport_>
void Rpc<Transport_>::handle_session_connect_resp(SessionMgmtPkt *sm_pkt) {
  assert(sm_pkt != NULL);
  assert(sm_pkt->pkt_type == SessionMgmtPktType::kConnectResp);
  assert(session_mgmt_is_valid_err_type(sm_pkt->err_type));

  /* Create the basic issue message using only the packet */
  char issue_msg[kMaxIssueMsgLen];
  sprintf(issue_msg,
          "eRPC Rpc: Rpc %s received connect response from %s for session %zu. "
          "Issue",
          get_name().c_str(), sm_pkt->server.name().c_str(),
          sm_pkt->client.session_num);

  /* Try to locate the requester session for this response */
  uint32_t session_num = sm_pkt->client.session_num;
  assert(session_num < session_vec.size());

  Session *session = session_vec[session_num];

  /*
   * Check if the client session was already disconnected. If so, the callback
   * is not invoked.
   */
  if (session == nullptr) {
    erpc_dprintf("%s: Client session is already disconnected.\n", issue_msg);
    return;
  }

  /*
   * The session is non-null. Ensure that the metadata that the client filled
   * in the connect request still matches.
   */
  assert(sm_pkt->server.app_tid == session->server.app_tid);
  assert(strcmp(sm_pkt->server.hostname, session->server.hostname) == 0);
  assert(memcmp((void *)&sm_pkt->client, (void *)&session->client,
                sizeof(sm_pkt->client)) == 0);

  /*
   * If we are here, we still have the requester session as Client.
   *
   * Check if the session state has advanced beyond kConnectInProgress. If so,
   * we are not interested in the response and the callback is not invoked.
   */
  assert(session->state >= SessionState::kConnectInProgress);

  if (session->state > SessionState::kConnectInProgress) {
    erpc_dprintf("%s: Client session is not in state %s.\n", issue_msg,
                 session_state_str(SessionState::kConnectInProgress).c_str());
    return;
  }

  /*
   * If the connect request failed, move the session to the error state and
   * invoke the callback.
   */
  if (sm_pkt->err_type != SessionMgmtErrType::kNoError) {
    erpc_dprintf("%s: Response type indicates error %s.\n", issue_msg,
                 session_mgmt_err_type_str(sm_pkt->err_type).c_str());

    session->state = SessionState::kError;
    session_mgmt_handler(session, SessionMgmtEventType::kConnectFailed,
                         sm_pkt->err_type, context);

    return;
  }

  /* Save server metadata, mark session connected, and invoke callback */
  session->server = sm_pkt->server;
  session->state = SessionState::kConnected;

  session_mgmt_handler(session, SessionMgmtEventType::kConnected,
                       SessionMgmtErrType::kNoError, context);
}

template <class Transport_>
void Rpc<Transport_>::handle_session_disconnect_req(SessionMgmtPkt *sm_pkt) {
  _unused(sm_pkt);
}

template <class Transport_>
void Rpc<Transport_>::handle_session_disconnect_resp(SessionMgmtPkt *sm_pkt) {
  _unused(sm_pkt);
}

}  // End ERpc
