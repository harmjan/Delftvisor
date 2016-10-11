#include "physical_switch.hpp"
#include "virtual_switch.hpp"
#include "slice.hpp"
#include "hypervisor.hpp"
#include "tag.hpp"

#include <boost/log/trivial.hpp>

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

	// Create the group that sends the packet back to the controller
	{
		fluid_msg::of13::GroupMod group_mod;
		group_mod.command(fluid_msg::of13::OFPGC_ADD);
		group_mod.group_type(fluid_msg::of13::OFPGT_INDIRECT);
		group_mod.group_id(0);

		// Create the bucket that forward
		fluid_msg::of13::Bucket bucket;
		bucket.weight(0);
		bucket.watch_port(fluid_msg::of13::OFPP_ANY);
		bucket.watch_group(fluid_msg::of13::OFPG_ANY);
		bucket.add_action(
			new fluid_msg::of13::OutputAction(
				fluid_msg::of13::OFPP_CONTROLLER,
				fluid_msg::of13::OFPCML_NO_BUFFER));
		group_mod.add_bucket(bucket);

		// Send the message
		send_message(group_mod);
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
			metadata_tag.set_group(false);
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
		send_message(flowmod_1);

		// Update shared link forwarding rules, the rules in table 1 with id 30
		auto needed_ports_it = needed_ports.find(port_no);
		if( needed_ports_it != needed_ports.end() ) {
			for( auto virtual_switch : needed_ports_it->second ) {
				fluid_msg::of13::FlowMod flowmod;
				flowmod.table_id(1);
				flowmod.priority(30);
				flowmod.buffer_id(OFP_NO_BUFFER);
				if( prev_state!=Port::State::link_rule &&
						current_state==Port::State::link_rule) {
					flowmod.command(fluid_msg::of13::OFPFC_ADD);
				}
				else if( prev_state==Port::State::link_rule &&
						current_state!=Port::State::link_rule ) {
					flowmod.command(fluid_msg::of13::OFPFC_DELETE);
				}
				else {
					continue;
				}

				// Create the match
				PortVLANTag vlan_tag;
				vlan_tag.set_port(VLANTag::max_port_id);
				vlan_tag.set_slice(virtual_switch->get_slice()->get_id());
				vlan_tag.add_to_match(flowmod);
				flowmod.add_oxm_field(
					new fluid_msg::of13::InPort(port_no));

				// Add the actions
				fluid_msg::of13::ApplyActions apply_actions;
				apply_actions.add_action(
					new fluid_msg::of13::PopVLANAction());
				flowmod.add_instruction(apply_actions);
				// Add the meter instruction
				// TODO Conditionally try to add meter instruction?
				//flowmod.add_instruction(
				//	new fluid_msg::of13::Meter(
				//		virtual_switch->get_slice()->get_id()+1));
				MetadataTag metadata_tag;
				metadata_tag.set_group(false);
				metadata_tag.set_virtual_switch(virtual_switch->get_id());
				metadata_tag.add_to_instructions(flowmod);
				flowmod.add_instruction(
					new fluid_msg::of13::GoToTable(2));

				// Send the message
				send_message( flowmod );
			}
		}
	}

	// TODO Update FLOOD port group

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

	// Loop over all virtual switches for which we have rewrite data
	for( auto& rewrite_entry_pair : rewrite_map ) {
		const int& virtual_switch_id        = rewrite_entry_pair.first;
		auto& rewrite_entry                 = rewrite_entry_pair.second;

		// Get the virtual switch for which we are going to update the
		const VirtualSwitch* virtual_switch =
			hypervisor->get_virtual_switch(virtual_switch_id);

		// Loop over all ports on the virtual switch
		for( auto& port_pair : virtual_switch->get_port_to_physical_switch() ) {
			const uint32_t& virtual_port  = port_pair.first;
			const uint64_t& physical_dpid = port_pair.second;

			// Retrieve more references and pointers so we can easily use
			// those below
			PhysicalSwitch::pointer physical_switch =
				hypervisor->get_physical_switch_by_datapath_id(physical_dpid);
			PhysicalSwitch::OutputGroup& output_group =
				rewrite_entry.output_groups.at(virtual_port);

			// Determine what state this rule should have
			PhysicalSwitch::OutputGroup::State new_state;
			uint32_t new_output_port;

			// If it is a port on this switch
			if( physical_dpid == features.datapath_id ) {
				// Retrieve the mapping from local to physical port id
				const auto& port_map = virtual_switch->get_port_map(features.datapath_id);

				// Get the physical port id
				new_output_port = port_map.get_physical(virtual_port);

				auto port_it = ports.find(new_output_port);
				// If the port is not yet found or to a host
				if( port_it==ports.end() || port_it->second.link==nullptr ) {
					new_state = PhysicalSwitch::OutputGroup::State::host_rule;
				}
				// If the port is to a shared link
				else {
					new_state = PhysicalSwitch::OutputGroup::State::shared_link_rule;
				}
			}
			// If it is a port on another switch that is 1 hop away
			else if(dist.at(physical_switch->get_id()) == 1) {
				new_state       = PhysicalSwitch::OutputGroup::State::switch_one_hop_rule;
				new_output_port = next.at(physical_switch->get_id());
			}
			// If it is a port on another switch
			else {
				new_state       = PhysicalSwitch::OutputGroup::State::switch_rule;
				new_output_port = next.at(physical_switch->get_id());
			}

			// If the states and output ports are the same the group doesn't
			// need to be updated
			if( output_group.state==new_state &&
				output_group.output_port==new_output_port ) {
				continue;
			}

			// Create the new group to
			fluid_msg::of13::GroupMod group_mod;
			// If this group doesn't exist it is an add command
			if( output_group.state == PhysicalSwitch::OutputGroup::State::no_rule ) {
				group_mod.command(fluid_msg::of13::OFPGC_ADD);
			}
			// Otherwise it is an edit command
			else {
				group_mod.command(fluid_msg::of13::OFPGC_MODIFY);
			}
			group_mod.group_type(fluid_msg::of13::OFPGT_INDIRECT);
			group_mod.group_id(output_group.group_id);

			// Update the state and output port in the output_group
			output_group.state       = new_state;
			output_group.output_port = new_output_port;

			// Create the bucket to add to the group mod
			fluid_msg::of13::Bucket bucket;
			bucket.weight(0);
			bucket.watch_port(fluid_msg::of13::OFPP_ANY);
			bucket.watch_group(fluid_msg::of13::OFPG_ANY);

			// Determine what actions to add to the bucket and do it
			fluid_msg::ActionSet action_set;
			if( new_state == PhysicalSwitch::OutputGroup::State::host_rule ) {
				action_set.add_action(
					new fluid_msg::of13::OutputAction(
						new_output_port,
						fluid_msg::of13::OFPCML_NO_BUFFER));
			}
			else if( new_state == PhysicalSwitch::OutputGroup::State::shared_link_rule ) {
				// Push the VLAN Tag
				action_set.add_action(
					new fluid_msg::of13::PushVLANAction(0x8100));

				// Set the data in the VLAN Tag
				PortVLANTag vlan_tag;
				vlan_tag.set_slice(virtual_switch->get_slice()->get_id());
				vlan_tag.set_port(VLANTag::max_port_id);
				vlan_tag.add_to_actions(action_set);

				// Output the packet over the proper port
				action_set.add_action(
					new fluid_msg::of13::OutputAction(
						new_output_port,
						fluid_msg::of13::OFPCML_NO_BUFFER));
			}
			else if( new_state == PhysicalSwitch::OutputGroup::State::switch_one_hop_rule ) {
				// Push the VLAN Tag
				action_set.add_action(
					new fluid_msg::of13::PushVLANAction(0x8100));

				// Get the port id on the foreign switch
				uint32_t foreign_output_port =
					virtual_switch
						->get_port_map(physical_dpid)
							.get_physical(virtual_port);

				// Set the data in the VLAN Tag
				PortVLANTag vlan_tag;
				vlan_tag.set_slice(virtual_switch->get_slice()->get_id());
				vlan_tag.set_port(foreign_output_port);
				vlan_tag.add_to_actions(action_set);

				// Output the packet over the proper port
				action_set.add_action(
					new fluid_msg::of13::OutputAction(
						new_output_port,
						fluid_msg::of13::OFPCML_NO_BUFFER));
			}
			// Otherwise the state needs to be switch_rule
			else {
				// Push the VLAN Tag
				action_set.add_action(
					new fluid_msg::of13::PushVLANAction(0x8100));

				// Get the port id on the foreign switch
				uint32_t foreign_output_port =
					virtual_switch
						->get_port_map(physical_dpid)
							.get_physical(virtual_port);

				// Set the data in the VLAN Tag
				PortVLANTag vlan_tag;
				vlan_tag.set_slice(virtual_switch->get_slice()->get_id());
				vlan_tag.set_port(foreign_output_port);
				vlan_tag.add_to_actions(action_set);

				// Output the packet over the proper port
				action_set.add_action(
					new fluid_msg::of13::GroupAction(
						get_forward_group_id(physical_switch.get())));
			}

			// Add the bucket
			bucket.actions(action_set);
			group_mod.add_bucket(bucket);

			// Send the message
			send_message(group_mod);
		}
	}

	// Loop over all switch forward groups and create/update them
	for( auto& switch_group_pair : switch_id_to_group_id ) {
		const int& switch_id = switch_group_pair.first;
		const uint32_t& new_output_port = next.at(switch_id);
		SwitchForwardGroup& switch_forward_group = switch_group_pair.second;

		fluid_msg::of13::GroupMod group_mod;
		// If this group doesn't exist it is an add command
		if( switch_forward_group.state == SwitchForwardGroup::State::no_rule ) {
			group_mod.command(fluid_msg::of13::OFPGC_ADD);
			switch_forward_group.state = SwitchForwardGroup::State::forward_rule;
		}
		// Otherwise it is an edit command
		else {
			// If the port hasn't changed there is no reason to send the
			// modify message
			if( new_output_port == switch_forward_group.output_port ) {
				continue;
			}

			group_mod.command(fluid_msg::of13::OFPGC_MODIFY);
		}
		group_mod.group_type(fluid_msg::of13::OFPGT_INDIRECT);
		group_mod.group_id(switch_forward_group.group_id);

		// Create the bucket to add to the group mod
		fluid_msg::of13::Bucket bucket;
		bucket.weight(0);
		bucket.watch_port(fluid_msg::of13::OFPP_ANY);
		bucket.watch_group(fluid_msg::of13::OFPG_ANY);

		fluid_msg::ActionSet action_set;
		// Push the VLAN Tag
		action_set.add_action(
			new fluid_msg::of13::PushVLANAction(0x8100));
		// Set the data in the VLAN Tag
		SwitchVLANTag vlan_tag;
		vlan_tag.set_switch(switch_id);
		vlan_tag.add_to_actions(action_set);
		// Output the packet over the proper port
		action_set.add_action(
			new fluid_msg::of13::OutputAction(
				new_output_port,
				fluid_msg::of13::OFPCML_NO_BUFFER));

		// Add the bucket
		bucket.actions(action_set);
		group_mod.add_bucket(bucket);

		send_message(group_mod);
	}
}

uint32_t PhysicalSwitch::get_forward_group_id(
		const PhysicalSwitch* physical_switch) {
	uint32_t group_id;
	auto switch_group_it = switch_id_to_group_id.find(physical_switch->get_id());
	if( switch_group_it == switch_id_to_group_id.end() ) {
		group_id = group_id_allocator.new_id();
		SwitchForwardGroup& switch_forward_group =
			switch_id_to_group_id[physical_switch->get_id()];
		switch_forward_group.group_id = group_id;
		switch_forward_group.state    = SwitchForwardGroup::State::no_rule;
	}
	else {
		group_id = switch_group_it->second.group_id;
	}
	return group_id;
}
