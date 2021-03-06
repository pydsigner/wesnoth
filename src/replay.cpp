/*
   Copyright (C) 2003 - 2015 by David White <dave@whitevine.net>
   Part of the Battle for Wesnoth Project http://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

/**
 *  @file
 *  Replay control code.
 *
 *  See http://www.wesnoth.org/wiki/ReplayWML for more info.
 */

#include "global.hpp"
#include "replay.hpp"

#include "actions/undo.hpp"
#include "config_assign.hpp"
#include "dialogs.hpp"
#include "display_chat_manager.hpp"
#include "game_display.hpp"
#include "game_preferences.hpp"
#include "game_data.hpp"
#include "log.hpp"
#include "map_label.hpp"
#include "map_location.hpp"
#include "play_controller.hpp"
#include "synced_context.hpp"
#include "resources.hpp"
#include "statistics.hpp"
#include "unit.hpp"
#include "whiteboard/manager.hpp"
#include "replay_recorder_base.hpp"

#include <boost/foreach.hpp>
#include <boost/lexical_cast.hpp>
#include <set>
#include <map>

static lg::log_domain log_replay("replay");
#define DBG_REPLAY LOG_STREAM(debug, log_replay)
#define LOG_REPLAY LOG_STREAM(info, log_replay)
#define WRN_REPLAY LOG_STREAM(warn, log_replay)
#define ERR_REPLAY LOG_STREAM(err, log_replay)

static lg::log_domain log_random("random");
#define DBG_RND LOG_STREAM(debug, log_random)
#define LOG_RND LOG_STREAM(info, log_random)
#define WRN_RND LOG_STREAM(warn, log_random)
#define ERR_RND LOG_STREAM(err, log_random)


//functions to verify that the unit structure on both machines is identical

static void verify(const unit_map& units, const config& cfg) {
	std::stringstream errbuf;
	LOG_REPLAY << "verifying unit structure...\n";

	const size_t nunits = cfg["num_units"].to_size_t();
	if(nunits != units.size()) {
		errbuf << "SYNC VERIFICATION FAILED: number of units from data source differ: "
			   << nunits << " according to data source. " << units.size() << " locally\n";

		std::set<map_location> locs;
		BOOST_FOREACH(const config &u, cfg.child_range("unit"))
		{
			const map_location loc(u);
			locs.insert(loc);

			if(units.count(loc) == 0) {
				errbuf << "data source says there is a unit at "
					   << loc << " but none found locally\n";
			}
		}

		for(unit_map::const_iterator j = units.begin(); j != units.end(); ++j) {
			if (locs.count(j->get_location()) == 0) {
				errbuf << "local unit at " << j->get_location()
					   << " but none in data source\n";
			}
		}
		replay::process_error(errbuf.str());
		errbuf.clear();
	}

	BOOST_FOREACH(const config &un, cfg.child_range("unit"))
	{
		const map_location loc(un);
		const unit_map::const_iterator u = units.find(loc);
		if(u == units.end()) {
			errbuf << "SYNC VERIFICATION FAILED: data source says there is a '"
				   << un["type"] << "' (side " << un["side"] << ") at "
				   << loc << " but there is no local record of it\n";
			replay::process_error(errbuf.str());
			errbuf.clear();
		}

		config cfg;
		u->write(cfg);

		bool is_ok = true;
		static const std::string fields[] = {"type","hitpoints","experience","side",""};
		for(const std::string* str = fields; str->empty() == false; ++str) {
			if (cfg[*str] != un[*str]) {
				errbuf << "ERROR IN FIELD '" << *str << "' for unit at "
					   << loc << " data source: '" << un[*str]
					   << "' local: '" << cfg[*str] << "'\n";
				is_ok = false;
			}
		}

		if(!is_ok) {
			errbuf << "(SYNC VERIFICATION FAILED)\n";
			replay::process_error(errbuf.str());
			errbuf.clear();
		}
	}

	LOG_REPLAY << "verification passed\n";
}

static time_t get_time(const config &speak)
{
	time_t time;
	if (!speak["time"].empty())
	{
		std::stringstream ss(speak["time"].str());
		ss >> time;
	}
	else
	{
		//fallback in case sender uses wesnoth that doesn't send timestamps
		time = ::time(NULL);
	}
	return time;
}

chat_msg::chat_msg(const config &cfg)
	: color_()
	, nick_()
	, text_(cfg["message"].str())
{
	const std::string& team_name = cfg["team_name"];
	if(team_name == "")
	{
		nick_ = cfg["id"].str();
	} else {
		nick_ = str_cast("*")+cfg["id"].str()+"*";
	}
	int side = cfg["side"].to_int(0);
	LOG_REPLAY << "side in message: " << side << std::endl;
	if (side==0) {
		color_ = "white";//observers
	} else {
		color_ = team::get_side_highlight_pango(side-1);
	}
	time_ = get_time(cfg);
	/*
	} else if (side==1) {
		color_ = "red";
	} else if (side==2) {
		color_ = "blue";
	} else if (side==3) {
		color_ = "green";
	} else if (side==4) {
		color_ = "purple";
		}*/
}

chat_msg::~chat_msg()
{
}

replay::replay(replay_recorder_base& base)
	: base_(&base)
	, message_locations()
{}
/*
	TODO: there should be different types of OOS messages:
		1)the normal OOS message
		2) the 'is guarenteed you'll get an assertion error after this and therefore you cannot continur' OOS message
		3) the 'do you want to overwrite calculated data with the data stored in replay' OOS error message.

*/
void replay::process_error(const std::string& msg)
{
	ERR_REPLAY << msg << std::flush;

	resources::controller->process_oos(msg); // might throw quit_game_exception()
}

void replay::add_unit_checksum(const map_location& loc,config& cfg)
{
	if(! game_config::mp_debug) {
		return;
	}
	config& cc = cfg.add_child("checksum");
	loc.write(cc);
	unit_map::const_iterator u = resources::units->find(loc);
	assert(u.valid());
	cc["value"] = get_checksum(*u);
}


void replay::init_side()
{
	config& cmd = add_command();
	config init_side;
	init_side["side_number"] = resources::controller->current_side();
	cmd.add_child("init_side", init_side);
}

void replay::add_start()
{
	config& cmd = add_command();
	cmd["sent"] = true;
	cmd.add_child("start");
}

void replay::add_countdown_update(int value, int team)
{
	config& cmd = add_command();
	config val;
	val["value"] = value;
	val["team"] = team;
	cmd.add_child("countdown_update",val);
}
void replay::add_synced_command(const std::string& name, const config& command)
{
	config& cmd = add_command();
	cmd.add_child(name,command);
	cmd["from_side"] = resources::controller->current_side();
	LOG_REPLAY << "add_synced_command: \n" << cmd.debug() << "\n";
}



void replay::user_input(const std::string &name, const config &input, int from_side)
{
	config& cmd = add_command();
	cmd["dependent"] = true;
	if(from_side == -1)
	{
		cmd["from_side"] = "server";
	}
	else
	{
		cmd["from_side"] = from_side;
	}
	cmd.add_child(name, input);
}

void replay::add_label(const terrain_label* label)
{
	assert(label);
	config& cmd = add_nonundoable_command();
	config val;

	label->write(val);

	cmd.add_child("label",val);
}

void replay::clear_labels(const std::string& team_name, bool force)
{
	config& cmd = add_nonundoable_command();

	config val;
	val["team_name"] = team_name;
	val["force"] = force;
	cmd.add_child("clear_labels",val);
}

void replay::add_rename(const std::string& name, const map_location& loc)
{
	config& cmd = add_command();
	cmd["async"] = true; // Not undoable, but depends on moves/recruits that are
	config val;
	loc.write(val);
	val["name"] = name;
	cmd.add_child("rename", val);
}


void replay::end_turn()
{
	config& cmd = add_command();
	cmd.add_child("end_turn");
}


void replay::add_log_data(const std::string &key, const std::string &var)
{
	config& ulog = base_->get_upload_log();
	ulog[key] = var;
}

void replay::add_log_data(const std::string &category, const std::string &key, const std::string &var)
{
	config& ulog = base_->get_upload_log();
	config& cat = ulog.child_or_add(category);
	cat[key] = var;
}

void replay::add_log_data(const std::string &category, const std::string &key, const config &c)
{
	config& ulog = base_->get_upload_log();
	config& cat = ulog.child_or_add(category);
	cat.add_child(key,c);
}

void replay::add_chat_message_location()
{
	message_locations.push_back(base_->get_pos() - 1);
}

void replay::speak(const config& cfg)
{
	config& cmd = add_nonundoable_command();
	cmd.add_child("speak",cfg);
	add_chat_message_location();
}

void replay::add_chat_log_entry(const config &cfg, std::back_insert_iterator<std::vector<chat_msg> > &i) const
{
	if (!cfg) return;

	if (!preferences::parse_should_show_lobby_join(cfg["id"], cfg["message"])) return;
	if (preferences::is_ignored(cfg["id"])) return;
	*i = chat_msg(cfg);
}

void replay::remove_command(int index)
{
	base_->remove_command(index);
	std::vector<int>::reverse_iterator loc_it;
	for (loc_it = message_locations.rbegin(); loc_it != message_locations.rend() && index < *loc_it;++loc_it)
	{
		--(*loc_it);
	}
}

// cached message log
static std::vector< chat_msg > message_log;


const std::vector<chat_msg>& replay::build_chat_log()
{
	std::vector<int>::iterator loc_it;
	int last_location = 0;
	std::back_insert_iterator<std::vector < chat_msg > > chat_log_appender( back_inserter(message_log));
	for (loc_it = message_locations.begin(); loc_it != message_locations.end(); ++loc_it)
	{
		last_location = *loc_it;

		const config &speak = command(last_location).child("speak");
		assert(speak);
		add_chat_log_entry(speak, chat_log_appender);

	}
	message_locations.clear();
	return message_log;
}

config replay::get_data_range(int cmd_start, int cmd_end, DATA_TYPE data_type)
{
	config res;

	for (int cmd = cmd_start; cmd < cmd_end; ++cmd)
	{
		config &c = command(cmd);
		//prevent creating 'blank' attribute values during checks
		const config &cc = c;
		if ((data_type == ALL_DATA || !cc["undo"].to_bool(true)) && !cc["sent"].to_bool(false))
		{
			res.add_child("command", c);
			if (data_type == NON_UNDO_DATA) c["sent"] = true;
		}
	}

	return res;
}

void replay::redo(const config& cfg)
{
	assert(base_->get_pos() == ncommands());
	BOOST_FOREACH(const config &cmd, cfg.child_range("command"))
	{
		base_->add_child() = cmd;
	}
	base_->set_to_end();

}



config& replay::get_last_real_command()
{
	for (int cmd_num = base_->get_pos() - 1; cmd_num >= 0; --cmd_num)
	{
		config &c = command(cmd_num);
		const config &cc = c;
		if (cc["dependent"].to_bool(false) || !cc["undo"].to_bool(true) || cc["async"].to_bool(false))
		{
			continue;
		}
		return c;
	}
	ERR_REPLAY << "replay::get_last_real_command called with no existent command." << std::endl;
	assert(false && "replay::get_last_real_command called with no existent command.");
	throw "replay::get_last_real_command called with no existent command.";
}
/// fixes a rename command when undoing a earlier command.
/// @return: true if the command should be removed.
static bool fix_rename_command(const config& c, config& async_child)
{
	if (const config &child = c.child("move"))
	{
		// A unit's move is being undone.
		// Repair unsynced cmds whose locations depend on that unit's location.
		std::vector<map_location> steps;

		try {
			read_locations(child,steps);
		} catch (bad_lexical_cast &) {
			WRN_REPLAY << "Warning: Path data contained something which could not be parsed to a sequence of locations:" << "\n config = " << child.debug() << std::endl;
		}

		if (steps.empty()) {
			ERR_REPLAY << "trying to undo a move using an empty path";
		}
		else {
			const map_location &src = steps.front();
			const map_location &dst = steps.back();
			map_location aloc(async_child);
			if (dst == aloc) src.write(async_child);
		}
	}
	else
	{
		const config *chld = &c.child("recruit");
		if (!*chld) chld = &c.child("recall");
		if (*chld) {
			// A unit is being un-recruited or un-recalled.
			// Remove unsynced commands that would act on that unit.
			map_location src(*chld);
			map_location aloc(async_child);
			if (src == aloc) {
				return true;
			}
		}
	}
	return false;
}

void replay::undo_cut(config& dst)
{
	assert(dst.empty());
	//assert that we are not undoing a command which we didn't execute yet.
	assert(at_end());

	//calculate the index of the last synced user action (which we want to undo).
	int cmd_index = ncommands() - 1;
	for (; cmd_index >= 0; --cmd_index)
	{
		//"undo"=no means speak/label/remove_label, especialy attack, recruits etc. have "undo"=yes
		//"async"=yes means rename_unit
		//"dependent"=true means user input
		const config &c = command(cmd_index);
		
		if(c["undo"].to_bool(true) && !c["async"].to_bool(false) && !c["dependent"].to_bool(false))
		{
			if(c["sent"].to_bool(false))
			{
				ERR_REPLAY << "trying to undo a command that was already sent.\n";
				return;
			}
			else
			{
				break;
			}
		}
	}

	if (cmd_index < 0)
	{
		ERR_REPLAY << "trying to undo a command but no command was found.\n";		
		return;
	}
	//Fix the [command]s after the undone action. This includes dependent commands for that user actions and async user action.
	for(int i = ncommands() - 1; i >= cmd_index; --i)
	{
		config &c = command(i);
		const config &cc = c;
		if(!cc["undo"].to_bool(true))
		{
			//Leave these commands on the replay.
		}
		else if(cc["async"].to_bool(false))
		{
			if(config& rename = c.child("rename"))
			{
				if(fix_rename_command(command(cmd_index), rename))
				{
					//remove the command from the replay if fix_rename_command requested it.
					remove_command(i);
				}
			}
		}
		else if(cc["dependent"].to_bool(false) || i == cmd_index)
		{
			//we loop backwars so we must insert new insert at beginning to preserve order.
			dst.add_child_at("command", config(), 0).swap(c);
			remove_command(i);
		}
		else
		{
			ERR_REPLAY << "Coudn't handle command:\n" << cc << "\nwhen undoing.\n";
		}
	}
	set_to_end();
}

void replay::undo()
{
	config dummy;
	undo_cut(dummy);
}

config &replay::command(int n)
{
	config & retv = base_->get_command_at(n);
	assert(retv);
	return retv;
}

int replay::ncommands() const
{
	return base_->size();
}

config& replay::add_command()
{
	//if we werent at teh end of teh replay we sould skip one or mutiple commands.
	assert(at_end());
	config& retv = base_->add_child();
	set_to_end();
	return retv;
}

config& replay::add_nonundoable_command()
{
	config& r = base_->insert_command(base_->get_pos());
	r["undo"] = false;
	base_->set_pos(base_->get_pos() + 1);
	return r;
}

void replay::start_replay()
{
	base_->set_pos(0);
}

void replay::revert_action()
{

	if (base_->get_pos() > 0)
		base_->set_pos(base_->get_pos() - 1);
}

config* replay::get_next_action()
{
	if (at_end())
		return NULL;

	LOG_REPLAY << "up to replay action " << base_->get_pos() + 1 << '/' << ncommands() << '\n';

	config* retv = &command(base_->get_pos());
	base_->set_pos(base_->get_pos() + 1);
	return retv;
}


bool replay::at_end() const
{
	assert(base_->get_pos() <= ncommands());
	return base_->get_pos() == ncommands();
}

void replay::set_to_end()
{
	base_->set_to_end();
}

void replay::clear()
{
	message_locations.clear();
	message_log.clear();
	//FIXME
}

bool replay::empty()
{
	return ncommands() == 0;
}

void replay::add_config(const config& cfg, MARK_SENT mark)
{
	BOOST_FOREACH(const config &cmd, cfg.child_range("command"))
	{
		config &cfg = base_->insert_command(base_->size());
		cfg = cmd;
		if(mark == MARK_AS_SENT) {
			cfg["sent"] = true;
		}
	}
}
bool replay::add_start_if_not_there_yet()
{
	//this method would confuse the value of 'pos' otherwise
	assert(base_->get_pos() == 0);
	//since pos is 0, at_end() is equivalent to empty()
	if(at_end() || !base_->get_command_at(0).has_child("start"))
	{
		base_->insert_command(0) = config_of("start", config())("sent", true);
		return true;
	}
	else
	{
		return false;
	}
}

static void show_oos_error_error_function(const std::string& message, bool /*heavy*/)
{
	replay::process_error(message);
}

REPLAY_RETURN do_replay(bool one_move)
{
	log_scope("do replay");

	if (!resources::controller->is_skipping_replay()) {
		resources::screen->recalculate_minimap();
	}

	update_locker lock_update(resources::screen->video(), resources::controller->is_skipping_replay());
	return do_replay_handle(one_move);
}
/**
	@returns:
		if we expect a user choice and found something that prevents us from moving on we return REPLAY_FOUND_DEPENDENT (even if it is not a dependent command)
		else if we found an [end_turn] we return REPLAY_FOUND_END_TURN
		else if we found a player action and one_move=true we return REPLAY_FOUND_END_MOVE
		else (<=> we reached the end of the replay) we return REPLAY_RETURN_AT_END
*/
REPLAY_RETURN do_replay_handle(bool one_move)
{

	//team &current_team = (*resources::teams)[side_num - 1];

	const int side_num = resources::controller->current_side();
	while(true)
	{
		const config *cfg = resources::recorder->get_next_action();
		const bool is_synced = synced_context::is_synced();

		DBG_REPLAY << "in do replay with is_synced=" << is_synced << "\n";

		if (cfg != NULL)
		{
			DBG_REPLAY << "Replay data:\n" << *cfg << "\n";
		}
		else
		{
			DBG_REPLAY << "Replay data at end\n";
			return REPLAY_RETURN_AT_END;
		}


		const config::all_children_itors ch_itors = cfg->all_children_range();
		//if there is an empty command tag or a start tag
		if (ch_itors.first == ch_itors.second || cfg->has_child("start"))
		{
			//this shouldn't happen anymore because replaycontroller now moves over the [start] with get_next_action
			//also we removed the the "add empty replay entry at scenario reload" behavior.
			ERR_REPLAY << "found "<<  cfg->debug() <<" in replay" << std::endl;
			//do nothing
		}
		else if (const config &child = cfg->child("speak"))
		{
			const std::string &team_name = child["team_name"];
			const std::string &speaker_name = child["id"];
			const std::string &message = child["message"];
			//if (!preferences::parse_should_show_lobby_join(speaker_name, message)) return;
			bool is_whisper = (speaker_name.find("whisper: ") == 0);
			resources::recorder->add_chat_message_location();
			if (!resources::controller->is_skipping_replay() || is_whisper) {
				int side = child["side"];
				resources::screen->get_chat_manager().add_chat_message(get_time(child), speaker_name, side, message,
						(team_name.empty() ? events::chat_handler::MESSAGE_PUBLIC
						: events::chat_handler::MESSAGE_PRIVATE),
						preferences::message_bell());
			}
		}
		else if (const config &child = cfg->child("label"))
		{
			terrain_label label(resources::screen->labels(), child);

			resources::screen->labels().set_label(label.location(),
						label.text(),
						label.team_name(),
						label.color());
		}
		else if (const config &child = cfg->child("clear_labels"))
		{
			resources::screen->labels().clear(std::string(child["team_name"]), child["force"].to_bool());
		}
		else if (const config &child = cfg->child("rename"))
		{
			const map_location loc(child);
			const std::string &name = child["name"];

			unit_map::iterator u = resources::units->find(loc);
			if (u.valid() && !u->unrenamable()) {
				u->rename(name);
			} else {
				// Users can rename units while it's being killed or at another machine.
				// This since the player can rename units when it's not his/her turn.
				// There's not a simple way to prevent that so in that case ignore the
				// rename instead of throwing an OOS.
				// The same way it is possible that an unrenamable unit moves to a
				// hex where previously a renamable unit was.
				WRN_REPLAY << "attempt to rename unit at location: "
				   << loc << (u.valid() ? ", which is unrenamable" : ", where none exists (anymore)") << "\n";
			}
		}

		else if (cfg->child("init_side"))
		{

			if(is_synced)
			{
				replay::process_error("found side initialization in replay expecting a user choice\n" );
				resources::recorder->revert_action();
				return REPLAY_FOUND_DEPENDENT;
			}
			else
			{
				resources::controller->do_init_side();
			}
		}

		//if there is an end turn directive
		else if (cfg->child("end_turn"))
		{
			if(is_synced)
			{
				replay::process_error("found turn end in replay while expecting a user choice\n" );
				resources::recorder->revert_action();
				return REPLAY_FOUND_DEPENDENT;
			}
			else
			{
				if (const config &child = cfg->child("verify")) {
					verify(*resources::units, child);
				}

				return REPLAY_FOUND_END_TURN;
			}
		}
		else if (const config &child = cfg->child("countdown_update"))
		{
			int val = child["value"];
			int tval = child["team"];
			if (tval <= 0  || tval > int(resources::teams->size())) {
				std::stringstream errbuf;
				errbuf << "Illegal countdown update \n"
					<< "Received update for :" << tval << " Current user :"
					<< side_num << "\n" << " Updated value :" << val;

				replay::process_error(errbuf.str());
			} else {
				(*resources::teams)[tval - 1].set_countdown_time(val);
			}
		}
		else if ((*cfg)["dependent"].to_bool(false))
		{
			if(!is_synced)
			{
				replay::process_error("found dependent command in replay while is_synced=false\n" );
				//ignore this command
				continue;
			}
			//this means user choice.
			// it never makes sense to try to execute a user choice.
			// but we are called from
			// the only other option for "dependent" command is checksum wich is already checked.
			assert(cfg->all_children_count() == 1);
			std::string child_name = cfg->all_children_range().first->key;
			DBG_REPLAY << "got an dependent action name = " << child_name <<"\n";
			resources::recorder->revert_action();
			return REPLAY_FOUND_DEPENDENT;
		}
		else
		{
			//we checked for empty commands at the beginning.
			const std::string & commandname = cfg->ordered_begin()->key;
			config data = cfg->ordered_begin()->cfg;

			if(is_synced)
			{
				replay::process_error("found [" + commandname + "] command in replay expecting a user choice\n" );
				resources::recorder->revert_action();
				return REPLAY_FOUND_DEPENDENT;
			}
			else
			{
				LOG_REPLAY << "found commandname " << commandname << "in replay";
				
				if((*cfg)["from_side"].to_int(0) != resources::controller->current_side()) {
					ERR_REPLAY << "recieved a synced [command] from side " << (*cfg)["from_side"].to_int(0) << ". Expacted was a [command] from side " << resources::controller->current_side() << "\n";
				}
				else if((*cfg)["side_invalid"].to_bool(false)) {
					ERR_REPLAY << "recieved a synced [command] from side " << (*cfg)["from_side"].to_int(0) << ". Sended from wrong client.\n";
				}
				/*
					we need to use the undo stack during replays in order to make delayed shroud updated work.
				*/
				synced_context::run(commandname, data, true, !resources::controller->is_skipping_replay(), show_oos_error_error_function);
				if(resources::controller->is_regular_game_end()) {
					return REPLAY_FOUND_END_LEVEL;
				}
				if (one_move) {
					return REPLAY_FOUND_END_MOVE;
				}
			}
		}

		if (const config &child = cfg->child("verify")) {
			verify(*resources::units, child);
		}
	}
}

replay_network_sender::replay_network_sender(replay& obj) : obj_(obj), upto_(obj_.ncommands())
{
}

replay_network_sender::~replay_network_sender()
{
	try {
	commit_and_sync();
	} catch (...) {}
}

void replay_network_sender::sync_non_undoable()
{
	if(network::nconnections() > 0) {
		resources::whiteboard->send_network_data();

		config cfg;
		const config& data = cfg.add_child("turn",obj_.get_data_range(upto_,obj_.ncommands(),replay::NON_UNDO_DATA));
		if(data.empty() == false) {
			network::send_data(cfg, 0);
		}
	}
}

void replay_network_sender::commit_and_sync()
{
	if(network::nconnections() > 0) {
		resources::whiteboard->send_network_data();

		config cfg;
		const config& data = cfg.add_child("turn",obj_.get_data_range(upto_,obj_.ncommands()));

		if(data.empty() == false) {
			network::send_data(cfg, 0);
		}

		upto_ = obj_.ncommands();
	}
}


static std::map<int, config> get_user_choice_internal(const std::string &name, const mp_sync::user_choice &uch, const std::set<int>& sides)
{
	const int max_side  = static_cast<int>(resources::teams->size());

	BOOST_FOREACH(int side, sides)
	{
		//the caller has to ensure this.
		assert(1 <= side && side <= max_side);
		assert(!(*resources::teams)[side-1].is_empty());
	}


	//this should never change during the execution of this function.
	const int current_side = resources::controller->current_side();
	const bool is_mp_game = network::nconnections() != 0;
	// whether sides contains a side that is not the currently active side.
	const bool contains_other_side = !sides.empty() && (sides.size() != 1 || sides.find(current_side) == sides.end());
	if(contains_other_side)
	{
		synced_context::set_is_simultaneously();
	}
	std::map<int,config> retv;
	/*
		when we got all our answers we stop.
	*/
	while(retv.size() != sides.size())
	{
		/*
			there might be speak or similar commands in the replay before the user input.
		*/
		do_replay_handle();

		/*
			these value might change due to player left/reassign during pull_remote_user_input
		*/
		//equals to any side in sides that is local, 0 if no such side exists.
		int local_side = 0;
		//if for any side from which we need an answer
		BOOST_FOREACH(int side, sides)
		{
			//and we havent already received our answer from that side
			if(retv.find(side) == retv.end())
			{
				//and it is local
				if((*resources::teams)[side-1].is_local() && !(*resources::teams)[side-1].is_idle())
				{
					//then we have to make a local choice.
					local_side = side;
					break;
				}
			}
		}

		bool has_local_side = local_side != 0;
		bool is_replay_end = resources::recorder->at_end();

		if (is_replay_end && has_local_side)
		{
			leave_synced_context sync;
			/* At least one of the decisions is ours, and it will be inserted
			   into the replay. */
			DBG_REPLAY << "MP synchronization: local choice\n";
			config cfg = uch.query_user(local_side);

			resources::recorder->user_input(name, cfg, local_side);
			retv[local_side]= cfg;

			//send data to others.
			//but if there wasn't any data sended during this turn, we don't want to bein wth that now.
			if(synced_context::is_simultaneously() || current_side != local_side)
			{
				synced_context::send_user_choice();
			}
			continue;

		}
		else if(is_replay_end && !has_local_side)
		{
			//we are in a mp game, and the data has not been recieved yet.
			DBG_REPLAY << "MP synchronization: waiting for remote choice\n";

			assert(is_mp_game);
			synced_context::pull_remote_user_input();

			SDL_Delay(10);
			continue;
		}
		else if(!is_replay_end)
		{
			DBG_REPLAY << "MP synchronization: extracting choice from replay with has_local_side=" << has_local_side << "\n";

			const config *action = resources::recorder->get_next_action();
			assert(action); //action cannot be null because resources::recorder->at_end() returned false.
			if( !action->has_child(name) || !(*action)["dependent"].to_bool())
			{
				replay::process_error("[" + name + "] expected but none found\n. found instead:\n" + action->debug());
				//We save this action for later
				resources::recorder->revert_action();
				//and let the user try to get the intended result.
				BOOST_FOREACH(int side, sides)
				{
					if(retv.find(side) == retv.end())
					{
						retv[side] = uch.query_user(side);
					}
				}
				return retv;
			}
			int from_side = (*action)["from_side"].to_int(0);
			if ((*action)["side_invalid"].to_bool(false) == true)
			{
				//since this 'cheat' can have a quite heavy effect especialy in umc content we give an oos error .
				replay::process_error("MP synchronization: side_invalid in replay data, this could mean someone wants to cheat.\n");
			}
			if (sides.find(from_side) == sides.end())
			{
				replay::process_error("MP synchronization: we got an answer from side " + boost::lexical_cast<std::string>(from_side) + "for [" + name + "] which is not was we expected\n");
				continue;
			}
			if(retv.find(from_side) != retv.end())
			{
				replay::process_error("MP synchronization: we got already our answer from side " + boost::lexical_cast<std::string>(from_side) + "for [" + name + "] now we have it twice.\n");
			}
			retv[from_side] = action->child(name);
			continue;
		}
	}//while
	return retv;
}

std::map<int,config> mp_sync::get_user_choice_multiple_sides(const std::string &name, const mp_sync::user_choice &uch,
	std::set<int> sides)
{
	//pass sides by copy because we need a copy.
	const bool is_synced = synced_context::is_synced();
	const int max_side  = static_cast<int>(resources::teams->size());
	//we currently don't check for too early because luas sync choice doesn't necessarily show screen dialogs.
	//It (currently) in the responsibility of the user of sync choice to not use dialogs during prestart events..
	if(!is_synced)
	{
		//we got called from inside luas wesnoth.synchronize_choice or from a select event.
		replay::process_error("MP synchronization only works in a synced context (for example Select or preload events are no synced context).\n");
		return std::map<int,config>();
	}

	/*
		for empty sides we want to use random choice instead.
	*/
	std::set<int> empty_sides;
	BOOST_FOREACH(int side, sides)
	{
		assert(1 <= side && side <= max_side);
		if( (*resources::teams)[side-1].is_empty())
		{
			empty_sides.insert(side);
		}
	}

	BOOST_FOREACH(int side, empty_sides)
	{
		sides.erase(side);
	}

	std::map<int,config> retv =  get_user_choice_internal(name, uch, sides);

	BOOST_FOREACH(int side, empty_sides)
	{
		retv[side] = uch.random_choice(side);
	}
	return retv;

}

/*
	fixes some rare cases and calls get_user_choice_internal if we are in a synced context.
*/
config mp_sync::get_user_choice(const std::string &name, const mp_sync::user_choice &uch,
	int side)
{
	const bool is_too_early = resources::gamedata->phase() != game_data::START && resources::gamedata->phase() != game_data::PLAY;
	const bool is_synced = synced_context::is_synced();
	const bool is_mp_game = network::nconnections() != 0;//Only used in debugging output below
	const int max_side  = static_cast<int>(resources::teams->size());
	bool is_side_null_controlled;

	if(!is_synced)
	{
		//we got called from inside luas wesnoth.synchronize_choice or from a select event (or maybe a preload event?).
		//This doesn't cause problems and someone could use it for example to use a [message][option] inside a wesnoth.synchronize_choice which could be useful,
		//so just give a warning.
		LOG_REPLAY << "MP synchronization called during an unsynced context.\n";
		return uch.query_user(side);
	}
	if(is_too_early && uch.is_visible())
	{
		//We are in a prestart event or even earlier.
		//Although we are able to sync them, we cannot use query_user,
		//because we cannot (or shouldn't) put things on the screen inside a prestart event, this is true for SP and MP games.
		//Quotation form event wiki: "For things displayed on-screen such as character dialog, use start instead"
		return uch.random_choice(side);
	}
	//in start events it's unclear to decide on which side the function should be executed (default= side1 still).
	//But for advancements we can just decide on the side that owns the unit and that's in the responsibility of advance_unit_at.
	//For [message][option] and luas sync_choice the scenario designer is responsible for that.
	//For [get_global_variable] side is never null.

	/*
		side = 0 should default to the currently active side per definition.
	*/
	if ( side < 1  ||  max_side < side )
	{
		if(side != 0)
		{
			ERR_REPLAY << "Invalid parameter for side in get_user_choice." << std::endl;
		}
		side = resources::controller->current_side();
		LOG_REPLAY << " side changed to " << side << "\n";
	}
	is_side_null_controlled = (*resources::teams)[side-1].is_empty();

	LOG_REPLAY << "get_user_choice_called with"
			<< " name=" << name
			<< " is_synced=" << is_synced
			<< " is_mp_game=" << is_mp_game
			<< " is_side_null_controlled=" << is_side_null_controlled << "\n";

	if (is_side_null_controlled)
	{
		DBG_REPLAY << "MP synchronization: side 1 being null-controlled in get_user_choice.\n";
		//most likely we are in a start event with an empty side 1
		//but calling [set_global_variable] to an empty side might also cause this.
		//i think in that case we should better use uch.random_choice(),
		//which could return something like config_of("invalid", true);
		side = 1;
		while ( side <= max_side  &&  (*resources::teams)[side-1].is_empty() )
			side++;
		assert(side <= max_side);
	}


	assert(1 <= side && side <= max_side);

	std::set<int> sides;
	sides.insert(side);
	std::map<int, config> retv = get_user_choice_internal(name, uch, sides);
	if(retv.find(side) == retv.end())
	{
		//An error occured, get_user_choice_internal should have given an oos error message
		return config();
	}
	return retv[side];
}
