/**
 * Internal functions which implement the main SCP transport logic.
 */

#include <sys/socket.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#include <uv.h>

#include <rs.h>
#include <rs__internal.h>
#include <rs__scp.h>


void
rs__attempt_transmission(rs_conn_t *conn, rs__outstanding_t *os)
{
	// Don't do anything if the channel has been cancelled. Any required callbacks
	// have already been dealt with.
	if (!os->active)
		return;
	
	if (++os->n_tries <= conn->n_tries) {
		// Attempt to transmit
		os->send_req_active = true;
		if (uv_udp_send(&(os->send_req),
		                &(conn->udp_handle),
		                &(os->packet), 1,
		                conn->addr,
		                rs__udp_send_cb)) {
			// Transmission failiure: clean up
			os->send_req_active = false;
			rs__cancel_outstanding(conn, os, -1);
			return;
		}
	} else {
		// Maximum number of attempts made, fail and clean up.
		rs__cancel_outstanding(conn, os, -1);
	}
}


void
rs__timer_cb(uv_timer_t *handle)
{
	rs__outstanding_t *os = (rs__outstanding_t *)handle->data;
	
	// The packet didn't arrive, attempt retransmission (which will fail if done
	// too many times)
	rs__attempt_transmission(os->conn, os);
}


void
rs__udp_send_cb(uv_udp_send_t *req, int status)
{
	rs__outstanding_t *os = (rs__outstanding_t *)req->data;
	rs_conn_t *conn = os->conn;
	
	// Record that we've recieved the callback for the request
	os->send_req_active = false;
	
	// If we were waiting on this callback before freeing, we can now re-attempt
	// the free and quit.
	if (conn->free) {
		rs_free(conn);
		return;
	}
	
	// If we were waiting on this callback before the channel could be marked as
	// inactive after cancellation, do that.
	if (os->active && os->cancelled) {
		os->active = false;
		os->cancelled = false;
		
		// Now that the channel is nolonger active, we may potentially handle new
		// requests.
		rs__process_request_queue(conn);
		return;
	}
	
	// If something went wrong, cancel the request
	if (status != 0) {
		rs__cancel_outstanding(os->conn, os, -1);
		return;
	}
	
	// The packet has been dispatched, setup a timeout for the response
	uv_timer_start(&(os->timer_handle), rs__timer_cb, conn->timeout, 0);
}


void
rs__udp_recv_alloc_cb(uv_handle_t *handle,
                      size_t suggested_size, uv_buf_t *buf)
{
	// XXX: Just use malloc for now...
	buf->base = malloc(suggested_size);
	
	if (buf->base)
		buf->len = suggested_size;
	else
		buf->len = 0;
}


void
rs__udp_recv_cb(uv_udp_t *handle,
                ssize_t nread, const uv_buf_t *buf,
                const struct sockaddr *addr,
                unsigned int flags)
{
	rs_conn_t *conn = (rs_conn_t *)(handle->data);
	
	int i;
	
	// Ignore anything which isn't long enough to be an SCP packet. This also
	// skips cases when the length is 0 meaning "no more data" or <0 meaning some
	// kind of error has ocurred. Note that receive errors are rare and very
	// difficult to interpret so we consider them safe to ignore.
	if (nread >= RS__SIZEOF_SCP_PACKET(0, 0)) {
		// Check to see if a packet with this sequence number is outstanding (if
		// not, the packet is ignored too)
		uint16_t seq_num = rs__unpack_scp_packet_seq_num(*buf);
		for (i = 0; i < conn->n_outstanding; i++) {
			rs__outstanding_t *os = conn->outstanding + i;
			if (os->active && os->seq_num == seq_num) {
				uv_buf_t buf_ = *buf;
				buf_.len = nread;
				rs__process_response(conn, os, buf_);
				break;
			}
		}
	}
	
	// Free receive buffer
	if (buf->base)
		free(buf->base);
}