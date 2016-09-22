#include "physical_switch.hpp"
#include "virtual_switch.hpp"
#include "hypervisor.hpp"
#include "discoveredlink.hpp"
#include "slice.hpp"

#include "tag.hpp"

#include <boost/log/trivial.hpp>

PhysicalSwitch::PhysicalSwitch(
		boost::asio::ip::tcp::socket& socket,
		int id,
		Hypervisor* hypervisor)
	:
		OpenflowConnection::OpenflowConnection(socket),
		topology_discovery_timer(socket.get_io_service()),
		topology_discovery_port(0),
		id(id),
		hypervisor(hypervisor),
		state(unregistered) {
	// Set this one here already because the value is printed
	features.datapath_id = 0;
}

int PhysicalSwitch::get_id() const {
	return id;
}

const struct PhysicalSwitch::Features& PhysicalSwitch::get_features() const {
	return features;
}

const fluid_msg::of13::GroupFeatures& PhysicalSwitch::get_group_features() const {
	return group_features;
}

const fluid_msg::of13::MeterFeatures& PhysicalSwitch::get_meter_features() const {
	return meter_features;
}

const std::unordered_map<uint32_t,PhysicalSwitch::Port>& PhysicalSwitch::get_ports() const {
	return ports;
}

void PhysicalSwitch::register_port_interest(
		uint32_t port,
		boost::shared_ptr<VirtualSwitch> switch_pointer) {
	BOOST_LOG_TRIVIAL(trace) << *this << " interest was registered for port " << port;
	needed_ports[port].insert(switch_pointer);
}
void PhysicalSwitch::remove_port_interest(
		uint32_t port,
		boost::shared_ptr<VirtualSwitch> switch_pointer) {
	BOOST_LOG_TRIVIAL(trace) << *this << " interest was unregistered for port " << port;
	needed_ports.at(port).erase(switch_pointer);
}

void PhysicalSwitch::send_request_message(
		fluid_msg::OFMsg& message,
		boost::weak_ptr<VirtualSwitch> virtual_switch) {
	uint32_t xid = send_message(message);
	xid_map[xid].original_xid   = message.xid();
	xid_map[xid].virtual_switch = virtual_switch;
}

void PhysicalSwitch::start() {
	// Start up the generic connection handling
	OpenflowConnection::start();

	// Send an featuresrequest
	{
		fluid_msg::of13::FeaturesRequest features_message;
		send_message( features_message );
	}

	// Request information about the meters via multipart
	{
		fluid_msg::of13::MultipartRequestMeterFeatures information_message(
			0, // The xid will be set send_message
			0); // The only flag is the more the flag indicating more messages follow
		send_message( information_message );
	}

	// Request information about the groups via multipart
	{
		fluid_msg::of13::MultipartRequestGroupFeatures information_message(
			0, // The xid will be set send_message
			0); // The only flag is the more the flag indicating more messages follow
		send_message( information_message );
	}

	// Request ports via multipart
	{
		fluid_msg::of13::MultipartRequestPortDescription port_description_message(
			0, // The xid will be set send_message
			0); // The only flag is the more the flag indicating more messages follow
		send_message( port_description_message );
	}

	// Delete all the flow rules already in the switch
	{
		fluid_msg::of13::FlowMod flowmod;
		flowmod.command( fluid_msg::of13::OFPFC_DELETE );
		flowmod.table_id( fluid_msg::of13::OFPTT_ALL );
		flowmod.cookie_mask(0);
		flowmod.buffer_id(OFP_NO_BUFFER);
		send_message( flowmod );
	}

	// Send a barrier request to make sure the delete command
	// is executed before any new rules are added
	{
		fluid_msg::of13::BarrierRequest barrier;
		send_message(barrier);
	}

	// Create the rest of the initial rules
	create_static_rules();
	// Create the dynamic rules
	update_dynamic_rules();

	// Start sending topology discovery messages
	schedule_topology_discovery_message();

	BOOST_LOG_TRIVIAL(info) << *this << " started";
}

void PhysicalSwitch::stop() {
	// Stop the generic connection handling
	OpenflowConnection::stop();

	// Stop the topology discovery
	topology_discovery_timer.cancel();

	// Remove this switch from the registry
	if( state == unregistered ) {
		hypervisor->unregister_physical_switch(id);
	}
	else {
		hypervisor->unregister_physical_switch(features.datapath_id,id);
	}

	// Stop all the discovered links
	for( auto& port : ports ) {
		auto link = port.second.link;
		if( link != nullptr ) link->stop();
	}

	// Let the entire network recalculate, this is done to assure
	// that a virtual switch that only depended on this switch also
	// gets stopped.
	hypervisor->calculate_routes();

	BOOST_LOG_TRIVIAL(info) << *this << " stopped";
}

void PhysicalSwitch::create_static_rules() {
	// Create the topology discovery forward rule
	make_topology_discovery_rule();

	// Create the error detection rules
	{
		// Create the flowmod
		fluid_msg::of13::FlowMod flowmod;
		flowmod.command(fluid_msg::of13::OFPFC_ADD);
		flowmod.priority(0);
		flowmod.cookie(2);
		flowmod.table_id(0);
		flowmod.buffer_id(OFP_NO_BUFFER);

		// Create the actions
		fluid_msg::of13::WriteActions write_actions;
		write_actions.add_action(
			new fluid_msg::of13::OutputAction(
				fluid_msg::of13::OFPP_CONTROLLER,
				fluid_msg::of13::OFPCML_NO_BUFFER));
		flowmod.add_instruction(write_actions);

		// Send the message
		send_message(flowmod);

		// Change the table number and do it again
		flowmod.table_id(1);
		flowmod.cookie(3);
		send_message(flowmod);
	}

	// Create the rule forwarding packets that come from the
	// controller as if they arrived over a shared link
	{
		// Create the flowmod
		fluid_msg::of13::FlowMod flowmod;
		flowmod.command(fluid_msg::of13::OFPFC_ADD);
		flowmod.priority(10);
		flowmod.cookie(fluid_msg::of13::OFPP_CONTROLLER);
		flowmod.table_id(0);
		flowmod.buffer_id(OFP_NO_BUFFER);

		// Add the in-port match to flowmod
		flowmod.add_oxm_field(
			new fluid_msg::of13::InPort(fluid_msg::of13::OFPP_CONTROLLER));

		// Create the goto table instruction
		flowmod.add_instruction(
			new fluid_msg::of13::GoToTable(1));

		// Send the message
		send_message(flowmod);
	}

	// Create the meters per slice
	// TODO Conditionally try to add meters?
	// TODO Doesn't work with slices created after this physical switch
	for( const Slice& slice : hypervisor->get_slices() ) {
		fluid_msg::of13::MeterMod meter_mod;
		meter_mod.command(fluid_msg::of13::OFPMC_ADD);
		meter_mod.flags(fluid_msg::of13::OFPMF_PKTPS);
		meter_mod.meter_id(slice.get_id()+1); // TODO Document this better, meter id's start at 1
		meter_mod.add_band(
			new fluid_msg::of13::MeterBand(
				fluid_msg::of13::OFPMBT_DROP,
				slice.get_max_rate(),
				0)); // Burst needs to be 0 unless flag burst is used

		// Send the message
		send_message(meter_mod);
	}

	// TODO Send a barrierrequest
}

void PhysicalSwitch::update_dynamic_rules() {
	BOOST_LOG_TRIVIAL(info) << *this << " updating dynamic flow rules";

	// Update the port rules, there are 2 set of rules that are maintained
	// here. The rules in table 0 with priority 10 determining what to do
	// with packets that arrive over a certain link and the rules in table 1
	// with priority 10 determining what to do with packets that have
	// arrived over a link and want to be send out over a port on this switch.
	for( const auto& port_pair : ports ) {
		// Alias the values that are iterated over
		const uint32_t& port_no = port_pair.first;
		const Port& port = port_pair.second;

		// Start building the message to update table 0
		fluid_msg::of13::FlowMod flowmod_0;
		flowmod_0.priority(10);
		flowmod_0.cookie(port_no);
		flowmod_0.table_id(0);
		flowmod_0.buffer_id(OFP_NO_BUFFER);

		// If there is no rule about this port add a rule
		// in table 1
		fluid_msg::of13::FlowMod flowmod_1;
		flowmod_1.priority(10);
		flowmod_1.cookie(port_no);
		flowmod_1.table_id(1);
		flowmod_1.buffer_id(OFP_NO_BUFFER);

		// Determine what the current state of the forwarding rule
		// should be.
		Port::State current_state;
		// The virtual switch id in case this port has state host
		int virtual_switch_id;
		if( port.link != nullptr ) {
			// If this port has a link is that it's state
			current_state = Port::State::link_rule;
		}
		else {
			auto needed_it = needed_ports.find(port_no);
			if(
				needed_it!=needed_ports.end() &&
				needed_it->second.size()==1
			) {
				// A port can only be a host port if exactly 1 virtual
				// switch is interested in that port and it has no link
				current_state = Port::State::host_rule;
				// Extract the id of the virtual switch, there is
				// likely a better way to extract something from a set if
				// you know there is only 1 item, but this works
				virtual_switch_id = (*(needed_it->second.begin()))->get_id();
			}
			else {
				// In all other occasions this port should go down
				current_state = Port::State::drop_rule;
			}
		}

		// Look what state this port had previously
		Port::State prev_state = port.state;
		if( prev_state == Port::State::no_rule ) {
			// There is no rule known about this port
			flowmod_0.command(fluid_msg::of13::OFPFC_ADD);
			flowmod_1.command(fluid_msg::of13::OFPFC_ADD);
		}
		else {
			// If the state hasn't changed don't send any flowmod
			if( prev_state == current_state ) {
				continue;
			}
			flowmod_0.command(fluid_msg::of13::OFPFC_MODIFY);
			flowmod_1.command(fluid_msg::of13::OFPFC_MODIFY);
		}

		// Save the updated state
		ports[port_no].state = current_state;

		BOOST_LOG_TRIVIAL(info) << *this << " updating port rule for port "
			<< port_no << " to " << current_state;

		// Add the in-port match to flowmod_0
		flowmod_0.add_oxm_field(
			new fluid_msg::of13::InPort(port_no));

		// Add the necessary actions to flowmod_0
		if( current_state == Port::State::link_rule ) {
			flowmod_0.add_instruction(
				new fluid_msg::of13::GoToTable(1));
		}
		else if( current_state == Port::State::host_rule ) {
			// Add the meter instruction
			// TODO Conditionally try to add meter instruction?
			//flowmod_0.add_instruction(
			//	new fluid_msg::of13::Meter(
			//		slice_id+1));
			// Goto the tenant tables
			flowmod_0.add_instruction(
				new fluid_msg::of13::GoToTable(2));
			// Add the metadata write instruction
			MetadataTag metadata_tag;
			metadata_tag.set_group(0);
			metadata_tag.set_virtual_switch(virtual_switch_id);
			metadata_tag.add_to_instructions(flowmod_0);
		}
		else {
			// If current_state==Port::State::drop_rule don't add any actions
		}

		// Add the match to flowmod_1
		PortVLANTag vlan_tag;
		vlan_tag.set_port(port_no);
		vlan_tag.add_to_match(flowmod_1);

		// Set the actions for flowmod_1
		fluid_msg::of13::WriteActions write_actions;
		if( current_state == Port::State::host_rule ) {
			// Remove the VLAN Tag before forwarding to a host
			write_actions.add_action(
				new fluid_msg::of13::PopVLANAction());
		}
		else if( current_state == Port::State::link_rule ) {
			// Rewrite the port VLAN Tag to a shared link tag
			PortVLANTag vlan_tag;
			vlan_tag.set_port(VLANTag::max_port_id);
			vlan_tag.add_to_actions(write_actions);
		}
		// TODO What about drop rule?
		write_actions.add_action(
			new fluid_msg::of13::OutputAction(
				port_no,
				fluid_msg::of13::OFPCML_NO_BUFFER));
		flowmod_1.add_instruction(write_actions);

		// Send the flowmods
		send_message(flowmod_0);
		if( current_state != Port::State::drop_rule ) {
			send_message(flowmod_1);
		}

		// TODO Update FLOOD port group
	}

	// Figure out what to do with traffic meant for a different switch
	for( const auto& switch_it : hypervisor->get_physical_switches() ) {
		int other_id = switch_it.first;

		// Forwarding to this switch makes no sense
		if( other_id == id ) continue;

		// If there is no path to this switch
		const auto next_it    = next.find(other_id);
		const auto current_it = current_next.find(other_id);
		bool next_exists      = next_it!=next.end();
		bool current_exists   = current_it!=current_next.end();

		// If this switch was and is unreachable skip this switch
		if( !next_exists && !current_exists ) continue;
		// If this switch is reachable but that rule is already set
		// in the switch
		if(
			next_exists && current_exists &&
			next_it->second==current_it->second
		) continue;

		// If we arrived here we need to update something in the switch.
		// Create the flowmod
		fluid_msg::of13::FlowMod flowmod;
		flowmod.table_id(1);
		flowmod.priority(20);
		flowmod.buffer_id(OFP_NO_BUFFER);

		if( !current_exists ) {
			flowmod.command(fluid_msg::of13::OFPFC_ADD);
		}
		else if( current_exists && next_exists ) {
			flowmod.command(fluid_msg::of13::OFPFC_MODIFY);
		}
		else {
			flowmod.command(fluid_msg::of13::OFPFC_DELETE);
		}

		if( next_exists ) {
			// Add the vlantag match field
			SwitchVLANTag vlan_tag;
			vlan_tag.set_switch(other_id);
			vlan_tag.add_to_match(flowmod);

			// Tell the packet to output over the correct port
			fluid_msg::of13::WriteActions write_actions;
			write_actions.add_action(
				new fluid_msg::of13::OutputAction(
					next_it->second,
					fluid_msg::of13::OFPCML_NO_BUFFER));
			if( dist[other_id] == 1 ) {
				write_actions.add_action(
					new fluid_msg::of13::PopVLANAction());
			}
			flowmod.add_instruction(write_actions);
		}

		// Send the message
		send_message(flowmod);
	}
}

void PhysicalSwitch::handle_port( fluid_msg::of13::Port& port, uint8_t reason ) {
	// Create a port status update message
	fluid_msg::of13::PortStatus port_status_message;

	// Check if this is a new unknown port
	auto search = ports.find(port.port_no());
	if( search == ports.end() ) {
		if( reason == fluid_msg::of13::OFPPR_DELETE ) {
			// If this is a delete message for a port we didn't know about
			// don't do anything.
			return;
		}
		// This is a new port we didn't know about
		port_status_message.reason(fluid_msg::of13::OFPPR_ADD);
		// Create the port structure
		ports[port.port_no()].port_data = port;
		ports[port.port_no()].state     = Port::State::no_rule;
	}
	else {
		if( reason == fluid_msg::of13::OFPPR_DELETE ) {
			// Delete this port from the switch
			ports.erase(port.port_no());
			port_status_message.reason(fluid_msg::of13::OFPPR_DELETE);
		}
		else {
			port_status_message.reason(fluid_msg::of13::OFPPR_MODIFY);
		}
	}

	// Loop over the depended switches and make them check again
	auto switch_pointers = needed_ports.find(port.port_no());
	if( switch_pointers != needed_ports.end() ) {
		BOOST_LOG_TRIVIAL(trace) << *this << " PortStatus port=" << switch_pointers->first << " dep_sw_amount=" << switch_pointers->second.size();

		for( auto& switch_pointer : switch_pointers->second ) {
			// Skip if this virtual switch is not online
			if( !switch_pointer->is_connected() ) continue;

			BOOST_LOG_TRIVIAL(trace) << *this << "\tPortStatus dpid=" << features.datapath_id << ", port_no=" << port.port_no();

			// Rewrite the port number
			port.port_no(
				switch_pointer->
					get_virtual_port_no(features.datapath_id,port.port_no()));
			// Set the port data with the rewritten port number into the port status message
			port_status_message.desc( port );

			BOOST_LOG_TRIVIAL(trace) << *this << "\tPortStatus rewritten port_no=" << port.port_no();

			// Send the message to the virtual switch
			switch_pointer->send_message(port_status_message);
		}
	}
}

void PhysicalSwitch::reset_distances() {
	dist.clear();
	next.clear();

	set_distance(id,0);

	// Loop over the links and fill the dist and next maps
	for( const auto& port : ports ) {
		if( port.second.link != nullptr ) {
			int other_switch = port.second.link->get_other_switch_id(id);
			set_distance(other_switch,1);
			set_next(other_switch,port.first);
		}
	}
}
int PhysicalSwitch::get_distance(int switch_id) {
	// If we have no stored distance to switch_id the
	// distance is infinite
	if( dist.find(switch_id) == dist.end() ) {
		return topology::infinite;
	}
	else {
		return dist.at(switch_id);
	}
}
void PhysicalSwitch::set_distance(int switch_id, int distance) {
	dist[switch_id] = distance;
}
uint32_t PhysicalSwitch::get_next(int switch_id) {
	if( next.find(switch_id) == next.end() ) {
		BOOST_LOG_TRIVIAL(error) << "Asked next switch while no route is found";
		return UINT32_MAX;
	}
	else {
		return next.at(switch_id);
	}
}
void PhysicalSwitch::set_next(int switch_id, uint32_t port_number) {
	next[switch_id] = port_number;
}

bool PhysicalSwitch::rewrite_instruction_set(
		fluid_msg::of13::InstructionSet& old_instruction_set,
		fluid_msg::of13::InstructionSet& instruction_set_with_output,
		fluid_msg::of13::InstructionSet& instruction_set_without_output,
		VirtualSwitch* virtual_switch) {
	uint64_t metadata_tag  = 0;
	uint64_t metadata_mask = 0;

	// Loop over all the instructions in the original set
	for( fluid_msg::of13::Instruction* instruction : old_instruction_set.instruction_set() ) {
		if( instruction->type() == fluid_msg::of13::OFPIT_GOTO_TABLE ) {
			fluid_msg::of13::GoToTable* goto_table =
				(fluid_msg::of13::GoToTable*) instruction;

			// TODO Check if goto_table->table_id()+2 is within physical
			// switch capabilities

			instruction_set_with_output.add_instruction(
				new fluid_msg::of13::GoToTable(goto_table->table_id()+2));
			instruction_set_without_output.add_instruction(
				new fluid_msg::of13::GoToTable(goto_table->table_id()+2));
		}
		else if( instruction->type() == fluid_msg::of13::OFPIT_WRITE_METADATA ) {
			fluid_msg::of13::WriteMetadata* write_metadata =
				(fluid_msg::of13::WriteMetadata*) instruction;

			constexpr int total_bits = MetadataTag::num_virtual_switch_bits + 1;

			// Check if bits in the mask are set that would be shifted out
			constexpr uint64_t mask_check
				= make_mask(total_bits) << (64-total_bits);
			if( write_metadata->metadata_mask() & mask_check ) {
				BOOST_LOG_TRIVIAL(warning) << *this
					<< " metadata instruction uses reserved bits";
				return false;
			}

			// Add the shifted metadata tag information to the new tag
			metadata_tag  |= write_metadata->metadata()      << total_bits;
			metadata_mask |= write_metadata->metadata_mask() << total_bits;
		}
		else if( instruction->type() == fluid_msg::of13::OFPIT_WRITE_ACTIONS ) {
			fluid_msg::of13::WriteActions* write_actions =
				(fluid_msg::of13::WriteActions*) instruction;

			fluid_msg::ActionSet old_action_set = write_actions->actions();
			fluid_msg::ActionSet action_set_with_output, action_set_without_output;

			// Rewrite the action sets
			bool has_action_with_group = false;
			if( !rewrite_action_set(
					old_action_set,
					action_set_with_output,
					action_set_without_output,
					has_action_with_group,
					virtual_switch) ) {
				return false;
			}

			// If a group action was used set the metadata group bit
			if( has_action_with_group ) {
				metadata_tag  |= 1;
				metadata_mask |= 1;
			}

			// Create new instructions in the appropiate instruction sets
			// TODO What happens if an action set has no actions in it?
			instruction_set_with_output.add_instruction(
				new fluid_msg::of13::WriteActions(action_set_with_output));
			instruction_set_without_output.add_instruction(
				new fluid_msg::of13::WriteActions(action_set_without_output));
		}
		else if( instruction->type() == fluid_msg::of13::OFPIT_APPLY_ACTIONS ) {
			fluid_msg::of13::ApplyActions* apply_actions =
				(fluid_msg::of13::ApplyActions*) instruction;

			fluid_msg::ActionList old_action_list = apply_actions->actions();
			fluid_msg::ActionList new_action_list;

			// Rewrite the actions list
			if( !rewrite_action_list(
					old_action_list,
					new_action_list,
					virtual_switch) ) {
				return false;
			}

			// Create a new instruction in both instruction sets
			instruction_set_with_output.add_instruction(
				new fluid_msg::of13::ApplyActions(new_action_list));
			instruction_set_without_output.add_instruction(
				new fluid_msg::of13::ApplyActions(new_action_list));
		}
		else if( instruction->type() == fluid_msg::of13::OFPIT_CLEAR_ACTIONS ) {
			// Copy the instruction in the both action sets
			instruction_set_with_output.add_instruction(instruction->clone());
			instruction_set_without_output.add_instruction(instruction->clone());

			// Set the first bit in the metadata mask so the group bit gets
			// overwritten with a 0. If there also is a write action instruction
			// in this instruction set has the metadata_tag and metadata_mask
			// value already been set, the clear-action instruction is executed
			// before the write-action instruction. In that case the below statement
			// doesn't actually change anything which is correct.
			metadata_mask |= 1;
		}
		else if( instruction->type() == fluid_msg::of13::OFPIT_METER ) {
			BOOST_LOG_TRIVIAL(warning) << *this
				<< " received flowmod with meter instruction";
			return false;
		}
		else if( instruction->type() == fluid_msg::of13::OFPIT_EXPERIMENTER ) {
			BOOST_LOG_TRIVIAL(warning) << *this
				<< " received flowmod with experimenter instruction";
			return false;
		}
		else {
			// TODO Remove this case
			instruction_set_with_output.add_instruction(instruction->clone());
			instruction_set_without_output.add_instruction(instruction->clone());
		}
	}

	// If any information was set in the metadata mask we need to add
	// the metadata instruction to both instruction sets
	if( metadata_mask != 0 ) {
		instruction_set_with_output.add_instruction(
			new fluid_msg::of13::WriteMetadata(
				metadata_tag,
				metadata_mask));
		instruction_set_without_output.add_instruction(
			new fluid_msg::of13::WriteMetadata(
				metadata_tag,
				metadata_mask));
	}

	// Return that everything went ok
	return true;
}

bool PhysicalSwitch::rewrite_action_set(
		fluid_msg::ActionSet& old_action_set,
		fluid_msg::ActionSet& action_set_with_output,
		fluid_msg::ActionSet& action_set_without_output,
		bool& has_action_with_group,
		VirtualSwitch* virtual_switch) {
	// Loop over all actions
	for( fluid_msg::Action* action : old_action_set.action_set() ) {
		if( action->type() == fluid_msg::of13::OFPAT_OUTPUT ) {
			// TODO Wrong, this should be a group
			action_set_with_output.add_action(action->clone());
		}
		else if( action->type() == fluid_msg::of13::OFPAT_GROUP ) {
			// TODO Rewrite group number
			has_action_with_group = true;
			action_set_with_output.add_action(action->clone());
			action_set_without_output.add_action(action->clone());
		}
		else if( action->type() == fluid_msg::of13::OFPAT_SET_QUEUE ) {
			// Set queue actions are not supported yet
			BOOST_LOG_TRIVIAL(warning) << *this
				<< " received flowmod with set-queue in write-actions";
			return false;
		}
		else {
			// All other actions can be directly passed on to the switch
			action_set_with_output.add_action(action->clone());
			action_set_without_output.add_action(action->clone());
		}
	}

	return true;
}

bool PhysicalSwitch::rewrite_action_list(
		fluid_msg::ActionList& old_action_list,
		fluid_msg::ActionList& new_action_list,
		VirtualSwitch* virtual_switch) {
	for( fluid_msg::Action* action : old_action_list.action_list() ) {
		if( action->type() == fluid_msg::of13::OFPAT_OUTPUT ) {
			// TODO Rewrite output port
			new_action_list.add_action(action->clone());
		}
		else if( action->type() == fluid_msg::of13::OFPAT_GROUP ) {
			// TODO Rewrite group number
			new_action_list.add_action(action->clone());
		}
		else if( action->type() == fluid_msg::of13::OFPAT_SET_QUEUE ) {
			// Set queue actions are not supported yet
			BOOST_LOG_TRIVIAL(warning) << *this
				<< " received flowmod with set-queue in apply-actions";
			return false;
		}
		else {
			// All other actions can be directly passed on to the switch
			new_action_list.add_action(action->clone());
		}
	}

	return true;
}

PhysicalSwitch::pointer PhysicalSwitch::shared_from_this() {
	return boost::static_pointer_cast<PhysicalSwitch>(
			OpenflowConnection::shared_from_this());
}

void PhysicalSwitch::print_to_stream(std::ostream& os) const {
	os << "[PhysicalSwitch id=" << id << ", dpid=" << features.datapath_id << "]";
}

void PhysicalSwitch::handle_error(fluid_msg::of13::Error& error_message) {
	BOOST_LOG_TRIVIAL(info) << *this
		<< " received error Type=" << error_message.err_type()
		<< " Code=" << error_message.code();
	// TODO
}

void PhysicalSwitch::handle_features_reply(fluid_msg::of13::FeaturesReply& features_reply_message) {
	BOOST_LOG_TRIVIAL(info) << *this << " received features_reply";

	if( state == registered ) {
		BOOST_LOG_TRIVIAL(error) << *this << " received features_reply while already registered";
	}

	features.datapath_id  = features_reply_message.datapath_id();
	features.n_buffers    = features_reply_message.n_buffers();
	features.n_tables     = features_reply_message.n_tables();
	features.capabilities = features_reply_message.capabilities();

	// Register this physical switch at the hypervisor
	hypervisor->register_physical_switch(features.datapath_id,id);
	state = registered;

	// This can potentially allow a virtual switch that only depends
	// on this switch to come online. Execute check_online for all
	// virtual switches.
	//for( Slice& s : hypervisor->get_slices() ) s.check_online();
	hypervisor->calculate_routes();
}

void PhysicalSwitch::handle_config_reply(fluid_msg::of13::GetConfigReply& config_reply_message) {
	BOOST_LOG_TRIVIAL(info) << *this << " received get_config_reply";

	features.flags         = config_reply_message.flags();
	features.miss_send_len = config_reply_message.miss_send_len();
}

void PhysicalSwitch::handle_barrier_reply(fluid_msg::of13::BarrierReply& barrier_reply_message) {
	BOOST_LOG_TRIVIAL(info) << *this << " received barrier_reply";

	// TODO
	// Figure out who requested this
	// Mark this switch as done
	// If all physical switches are done
	//   send BarrierReply
}

void PhysicalSwitch::handle_packet_in(fluid_msg::of13::PacketIn& packet_in_message) {
	// Extract the data of this message
	fluid_msg::of13::InPort* in_port_tlv =
		(fluid_msg::of13::InPort*) packet_in_message
			.get_oxm_field(fluid_msg::of13::OFPXMT_OFB_IN_PORT);
	uint32_t in_port = in_port_tlv->value();

	if( packet_in_message.table_id()==0 ) {
		// This packet was generated from the hypervisor reserved table

		if( packet_in_message.cookie() == 1 ) {
			// This packet in was generated by the topology discovery rule
			handle_topology_discovery_packet_in(packet_in_message);
		}
		else {
			BOOST_LOG_TRIVIAL(error) << *this
				<< " received packet in via error detection rule on port " << in_port;
		}
	}
	else {
		BOOST_LOG_TRIVIAL(info) << *this << " received packet_in on port " << in_port;

		// Look at the metadata pipeline field to figure
		// out from what slice this packetin was generated.
		// TODO
	}
}

void PhysicalSwitch::handle_flow_removed(fluid_msg::of13::FlowRemoved& flow_removed_message) {
	BOOST_LOG_TRIVIAL(info) << *this << " received flow_removed";
	// TODO
}
void PhysicalSwitch::handle_port_status(fluid_msg::of13::PortStatus& port_status_message) {
	BOOST_LOG_TRIVIAL(info) << *this << " received port_status";

	// The handle port function does all the important stuff
	fluid_msg::of13::Port port = port_status_message.desc();
	handle_port( port, port_status_message.reason() );

	// Potentially a new port was added, update the dynamic rules
	update_dynamic_rules();
}

void PhysicalSwitch::handle_multipart_reply_group_features(fluid_msg::of13::MultipartReplyGroupFeatures& multipart_reply_message) {
	BOOST_LOG_TRIVIAL(info) << *this << " received group features";
	if( (multipart_reply_message.features().types()&(1<<fluid_msg::of13::OFPGT_ALL))==0 ) {
		BOOST_LOG_TRIVIAL(error) << *this << " switch doesn't support ALL group type needed for hypervisor " << multipart_reply_message.features().types();
	}
	if( (multipart_reply_message.features().types()&(1<<fluid_msg::of13::OFPGT_INDIRECT))==0 ) {
		BOOST_LOG_TRIVIAL(error) << *this << " switch doesn't support INDIRECT group type needed for hypervisor";
	}
	// TODO Check if the switch supports all actions
	// that the hypervisor needs per group

	group_features = multipart_reply_message.features();
}

void PhysicalSwitch::handle_multipart_reply_meter_features(fluid_msg::of13::MultipartReplyMeterFeatures& multipart_reply_message) {
	BOOST_LOG_TRIVIAL(info) << *this << " received meter features";
	if( (multipart_reply_message.meter_features().band_types()&fluid_msg::of13::OFPMBT_DROP) == 0 ) {
		BOOST_LOG_TRIVIAL(error) << *this << " switch doesn't support drop meter band type";
	}
	if( multipart_reply_message.meter_features().max_meter() < hypervisor->get_slices().size() ) {
		BOOST_LOG_TRIVIAL(error) << *this << " switch doesn't support enough meters";
	}

	meter_features = multipart_reply_message.meter_features();
}

void PhysicalSwitch::handle_multipart_reply_port_desc(fluid_msg::of13::MultipartReplyPortDescription& multipart_reply_message) {
	BOOST_LOG_TRIVIAL(info) << *this << " received multipart reply port description";

	// Just act as if we received all the ports via PortStatus add messages
	for( fluid_msg::of13::Port& port : multipart_reply_message.ports() ) {
		handle_port( port, fluid_msg::of13::OFPPR_ADD );
	}

	// Add the rules dropping/forwarding traffic about these new ports
	update_dynamic_rules();
}
