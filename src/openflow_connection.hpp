#pragma once

#include <vector>
#include <queue>
#include <string>

#include <boost/asio.hpp>
#include <boost/enable_shared_from_this.hpp>
#include <boost/thread.hpp>

#include <fluid/of13msg.hh>

class OpenflowConnection : public boost::enable_shared_from_this<OpenflowConnection> {
private:
	/// The vector that stores new messages
	std::vector<uint8_t> message_buffer;
	/// Setup the wait to receive a message
	void start_receive_message();
	/// Receive the header of the openflow message
	void receive_header( const boost::system::error_code& error, std::size_t bytes_transferred );
	/// Receive the body of the openflow message
	void receive_body(const boost::system::error_code& error, std::size_t bytes_transferred);

	// TODO Look at http://www.boost.org/doc/libs/1_58_0/doc/html/atomic/usage_examples.html#boost_atomic.usage_examples.mp_queue , it does multi-producer 1 consumer queue without locks
	/// The mutex that protects the message queue
	boost::mutex send_queue_mutex;
	/// The queue of messages that need to be send
	/**
	 * The element at the front is the message currently
	 * being send.
	 */
	std::queue<std::vector<uint8_t>> send_queue;
	/// Send a message over this connection
	void send_message_queue_head();
	/// Handle a send message
	void handle_send_message(const boost::system::error_code& error, std::size_t bytes_transferred);

	/// A boolean to check if the echo request was answered
	bool echo_received;
	/// The timer that expires when an echo is due
	boost::asio::deadline_timer echo_timer;
	/// Schedule sending an echo message
	void schedule_echo_message();
	/// Send an echo request over this connection
	void send_echo_message();

protected:
	/// The boost socket object
	boost::asio::ip::tcp::socket socket;

	/// Add handlers for each message to handle, the symmetric messages
	/// are handled in this class
	void handle_hello       (fluid_msg::of13::Hello& hello_message);
	void handle_echo_request(fluid_msg::of13::EchoRequest& echo_request_message);
	void handle_echo_reply  (fluid_msg::of13::EchoReply& echo_reply_message);
	void handle_experimenter(fluid_msg::of13::Experimenter& experimenter_message);

	/// The handle functions that should be implemented by inheriting
	/// classes
	virtual void handle_error(fluid_msg::of13::Error& error_message) = 0;
	virtual void handle_features_request(fluid_msg::of13::FeaturesRequest& features_request_message) = 0;
	virtual void handle_features_reply  (fluid_msg::of13::FeaturesReply& features_reply_message) = 0;

	virtual void handle_config_request(fluid_msg::of13::GetConfigRequest& config_request_message) = 0;
	virtual void handle_config_reply  (fluid_msg::of13::GetConfigReply& config_reply_message) = 0;
	virtual void handle_set_config    (fluid_msg::of13::SetConfig& set_config_message) = 0;

	virtual void handle_barrier_request(fluid_msg::of13::BarrierRequest& barrier_request_message) = 0;
	virtual void handle_barrier_reply  (fluid_msg::of13::BarrierReply& barrier_reply_message) = 0;

	virtual void handle_packet_in (fluid_msg::of13::PacketIn& packet_in_message) = 0;
	virtual void handle_packet_out(fluid_msg::of13::PacketOut& packet_out_message) = 0;

	virtual void handle_flow_removed(fluid_msg::of13::FlowRemoved& flow_removed_message) = 0;
	virtual void handle_port_status(fluid_msg::of13::PortStatus& port_status_message) = 0;

	virtual void handle_flow_mod (fluid_msg::of13::FlowMod& flow_mod_message) = 0;
	virtual void handle_group_mod(fluid_msg::of13::GroupMod& group_mod_message) = 0;
	virtual void handle_port_mod (fluid_msg::of13::PortMod& port_mod_message) = 0;
	virtual void handle_table_mod(fluid_msg::of13::TableMod& table_mod_message) = 0;
	virtual void handle_meter_mod(fluid_msg::of13::MeterMod& meter_mod_message) = 0;

	virtual void handle_multipart_request(fluid_msg::of13::MultipartRequest& multipart_request_message) = 0;
	virtual void handle_multipart_reply  (fluid_msg::of13::MultipartReply& multipart_reply_message) = 0;

	virtual void handle_queue_config_request(fluid_msg::of13::QueueGetConfigRequest& queue_config_request) = 0;
	virtual void handle_queue_config_reply  (fluid_msg::of13::QueueGetConfigReply& queue_config_reply) = 0;

	virtual void handle_role_request(fluid_msg::of13::RoleRequest& role_request_message) = 0;
	virtual void handle_role_reply  (fluid_msg::of13::RoleReply& role_reply_message) = 0;

	virtual void handle_get_async_request(fluid_msg::of13::GetAsyncRequest& async_request_message) = 0;
	virtual void handle_get_async_reply  (fluid_msg::of13::GetAsyncReply& async_reply_message) = 0;
	virtual void handle_set_async        (fluid_msg::of13::SetAsync& set_async_message) = 0;

	/// Generate the next xid
	uint32_t get_next_xid();

	/// Construct a new openflow connection
	OpenflowConnection(boost::asio::io_service& io);
	/// Construct a new openflow connection from an existing socket
	OpenflowConnection(boost::asio::ip::tcp::socket& socket);
public:
	/// Start receiving and pinging this connection
	void start();
	/// Stop receiving and pinging this connection
	void stop();

	/// Send an openflow message over this connection
	// TODO Change type to libfluid message
	void send_message(fluid_msg::OFMsg& message);

	/// Print this connection to a stream
	virtual void print_to_stream(std::ostream& os) const = 0;
};

/// Explain how to print these objects
std::ostream& operator<<(std::ostream& os, const OpenflowConnection& con);
