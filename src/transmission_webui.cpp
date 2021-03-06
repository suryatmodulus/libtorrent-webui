/*

Copyright (c) 2012, Arvid Norberg
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:

    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in
      the documentation and/or other materials provided with the distribution.
    * Neither the name of the author nor the names of its
      contributors may be used to endorse or promote products derived
      from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

*/

#include "transmission_webui.hpp"
#include "json_util.hpp"
#include "disk_space.hpp"
#include "base64.hpp"
#include "auth_interface.hpp"
#include "auth.hpp"
#include "no_auth.hpp"

#include <string.h> // for strcmp()
#include <stdio.h>
#include <vector>
#include <map>
#include <cstdint>
#include <boost/tuple/tuple.hpp>
#include <boost/asio/error.hpp>

extern "C" {
#include "local_mongoose.h"
#include "jsmn.h"
}

#include "libtorrent/add_torrent_params.hpp"
#include "libtorrent/torrent_status.hpp"
#include "libtorrent/parse_url.hpp"
#include "libtorrent/torrent_info.hpp"
#include "libtorrent/announce_entry.hpp"
#include "libtorrent/torrent_handle.hpp"
#include "libtorrent/session.hpp"
#include "libtorrent/session_status.hpp"
#include "libtorrent/peer_info.hpp"
#include "libtorrent/socket_io.hpp" // for print_address
#include "libtorrent/io.hpp" // for read_int32
#include "libtorrent/magnet_uri.hpp" // for make_magnet_uri
#include "response_buffer.hpp" // for appendf
#include "torrent_post.hpp" // for parse_torrent_post
#include "escape_json.hpp" // for escape_json
#include "save_settings.hpp"

namespace libtorrent
{

void return_error(mg_connection* conn, char const* msg)
{
	mg_printf(conn, "HTTP/1.1 401 Invalid Request\r\n"
		"Content-Type: text/json\r\n"
		"Content-Length: %d\r\n\r\n"
		"{ \"result\": \"%s\" }", int(16 + strlen(msg)), msg);
}

void return_failure(std::vector<char>& buf, char const* msg, std::int64_t tag)
{
	buf.clear();
	appendf(buf, "{ \"result\": \"%s\", \"tag\": %" PRId64 "}", msg, tag);
}

struct method_handler
{
	char const* method_name;
	void (transmission_webui::*fun)(std::vector<char>&, jsmntok_t* args, std::int64_t tag
		, char* buffer, permissions_interface const* p);
};

static method_handler handlers[] =
{
	{"torrent-add", &transmission_webui::add_torrent },
	{"torrent-get", &transmission_webui::get_torrent },
	{"torrent-set", &transmission_webui::set_torrent },
	{"torrent-start", &transmission_webui::start_torrent },
	{"torrent-start-now", &transmission_webui::start_torrent_now },
	{"torrent-stop", &transmission_webui::stop_torrent },
	{"torrent-verify", &transmission_webui::verify_torrent },
	{"torrent-reannounce", &transmission_webui::reannounce_torrent },
	{"torrent-remove", &transmission_webui::remove_torrent},
	{"session-stats", &transmission_webui::session_stats},
	{"session-get", &transmission_webui::get_session},
	{"session-set", &transmission_webui::set_session},
};

void transmission_webui::handle_json_rpc(std::vector<char>& buf, jsmntok_t* tokens
	, char* buffer, permissions_interface const* p)
{
	// we expect a "method" in the top level
	jsmntok_t* method = find_key(tokens, buffer, "method", JSMN_STRING);
	if (method == NULL)
	{
		return_failure(buf, "missing method in request", -1);
		return;
	}

	bool handled = false;
	buffer[method->end] = 0;
	char const* m = &buffer[method->start];
	jsmntok_t* args = NULL;
	for (int i = 0; i < sizeof(handlers)/sizeof(handlers[0]); ++i)
	{
		if (strcmp(m, handlers[i].method_name)) continue;

		args = find_key(tokens, buffer, "arguments", JSMN_OBJECT);
		std::int64_t tag = find_int(tokens, buffer, "tag");
		handled = true;

		if (args) buffer[args->end] = 0;
//		printf("%s: %s\n", m, args ? buffer + args->start : "{}");

		(this->*handlers[i].fun)(buf, args, tag, buffer, p);
		break;
	}
	if (!handled)
		printf("Unhandled: %s: %s\n", m, args ? buffer + args->start : "{}");
}

void transmission_webui::add_torrent(std::vector<char>& buf, jsmntok_t* args
	, std::int64_t tag, char* buffer, permissions_interface const* p)
{
	if (!p->allow_add())
	{
		return_failure(buf, "permission denied", tag);
		return;
	}

	jsmntok_t* cookies = find_key(args, buffer, "cookies", JSMN_STRING);

	add_torrent_params params = m_params_model;
	std::string save_path = find_string(args, buffer, "download-dir");
	if (!save_path.empty())
		params.save_path = save_path;

	bool paused = find_bool(args, buffer, "paused");
	if (paused)
	{
		params.flags |= add_torrent_params::flag_paused;
		params.flags &= ~add_torrent_params::flag_auto_managed;
	}
	else
	{
		params.flags &= ~add_torrent_params::flag_paused;
		params.flags |= add_torrent_params::flag_auto_managed;
	}

	std::string url = find_string(args, buffer, "filename");
	if (url.substr(0, 7) == "http://"
		|| url.substr(0, 8) == "https://"
		|| url.substr(0, 7) == "magnet:")
	{
		params.url = url;
	}
	else if (!url.empty())
	{
		error_code ec;
		shared_ptr<torrent_info> ti = libtorrent::make_shared<torrent_info>(url, std::ref(ec), 0);
		if (ec)
		{
			return_failure(buf, ec.message().c_str(), tag);
			return;
		}
		params.ti = ti;
	}
	else
	{
		std::string metainfo = base64decode(find_string(args, buffer, "metainfo"));
		error_code ec;
		shared_ptr<torrent_info> ti = libtorrent::make_shared<torrent_info>(&metainfo[0], metainfo.size(), std::ref(ec), 0);
		if (ec)
		{
			return_failure(buf, ec.message().c_str(), tag);
			return;
		}
		params.ti = ti;
	}

	error_code ec;
	torrent_handle h = m_ses.add_torrent(params);
	if (ec)
	{
		return_failure(buf, ec.message().c_str(), tag);
		return;
	}

	torrent_status st = h.status(torrent_handle::query_name);

	appendf(buf, "{ \"result\": \"success\", \"tag\": %" PRId64 ", "
		"\"arguments\": { \"torrent-added\": { \"hashString\": \"%s\", "
		"\"id\": %u, \"name\": \"%s\"}}}"
		, tag, to_hex(st.info_hash.to_string()).c_str()
		, h.id(), escape_json(st.name).c_str());
}

char const* to_bool(bool b) { return b ? "true" : "false"; }

bool all_torrents(torrent_status const& s)
{
	return true;
}

std::uint32_t tracker_id(announce_entry const& ae)
{
	sha1_hash urlhash = hasher(ae.url.c_str(), ae.url.size()).final();
	return ae.tier
		+ (std::uint32_t(urlhash[0]) << 8)
		+ (std::uint32_t(urlhash[1]) << 16)
		+ (std::uint32_t(urlhash[2]) << 24);
}

int tracker_status(announce_entry const& ae, torrent_status const& ts)
{
	enum tracker_state_t
	{
		TR_TRACKER_INACTIVE = 0,
		TR_TRACKER_WAITING = 1,
		TR_TRACKER_QUEUED = 2,
		TR_TRACKER_ACTIVE = 3
	};

	if (ae.updating) return TR_TRACKER_ACTIVE;
	if (ts.paused) return TR_TRACKER_INACTIVE;
	if (ae.fails >= ae.fail_limit) return TR_TRACKER_INACTIVE;
	if (ae.verified && ae.start_sent) return TR_TRACKER_WAITING;
	return TR_TRACKER_QUEUED;
}

int torrent_tr_status(torrent_status const& ts)
{
	enum tr_status_t
	{
		TR_STATUS_STOPPED = 0,
		TR_STATUS_CHECK_WAIT = 1,
		TR_STATUS_CHECK = 2,
		TR_STATUS_DOWNLOAD_WAIT = 3,
		TR_STATUS_DOWNLOAD = 4,
		TR_STATUS_SEED_WAIT = 5,
		TR_STATUS_SEED = 6
	};
	int res = 0;

	if (ts.paused && !ts.auto_managed)
		return TR_STATUS_STOPPED;
	switch(ts.state)
	{
		case libtorrent::torrent_status::checking_resume_data:
			res = TR_STATUS_CHECK;
			break;
		case libtorrent::torrent_status::checking_files:
			if (ts.paused) return TR_STATUS_CHECK_WAIT;
			else return TR_STATUS_CHECK;
			break;
		case libtorrent::torrent_status::downloading_metadata:
		case libtorrent::torrent_status::downloading:
		case libtorrent::torrent_status::allocating:
			if (ts.paused) return TR_STATUS_DOWNLOAD_WAIT;
			else return TR_STATUS_DOWNLOAD;
			break;
		case libtorrent::torrent_status::seeding:
		case libtorrent::torrent_status::finished:
			if (ts.paused) return TR_STATUS_SEED_WAIT;
			else return TR_STATUS_SEED;
		default:
			TORRENT_ASSERT(false);
	}
	return TR_STATUS_STOPPED;
}

int tr_file_priority(int prio)
{
	enum
	{
		TR_PRI_LOW = -1,
		TR_PRI_NORMAL =  0,
		TR_PRI_HIGH =  1
	};
	if (prio == 1) return TR_PRI_LOW;
	if (prio > 2) return TR_PRI_HIGH;
	return TR_PRI_NORMAL;
}

void transmission_webui::parse_ids(std::set<std::uint32_t>& torrent_ids, jsmntok_t* args, char* buffer)
{
	jsmntok_t* ids_ent = find_key(args, buffer, "ids", JSMN_ARRAY);
	if (ids_ent)
	{
		int num_ids = ids_ent->size;
		for (int i = 0; i < num_ids; ++i)
		{
			jsmntok_t* item = &ids_ent[i+1];
			torrent_ids.insert(atoi(buffer + item->start));
		}
	}
	else
	{
		std::int64_t id = find_int(args, buffer, "ids");
		if (id == 0) return;
		torrent_ids.insert(torrent_ids.begin(), id);
	}
}

void transmission_webui::get_torrent(std::vector<char>& buf, jsmntok_t* args
	, std::int64_t tag, char* buffer, permissions_interface const* p)
{
	if (!p->allow_list())
	{
		return_failure(buf, "permission denied", tag);
		return;
	}
	jsmntok_t* field_ent = find_key(args, buffer, "fields", JSMN_ARRAY);
	if (field_ent == NULL)
	{
		return_failure(buf, "missing 'field' argument", tag);
		return;
	}

	std::set<std::string> fields;
	int num_fields = field_ent->size;
	for (int i = 0; i < num_fields; ++i)
	{
		jsmntok_t* item = &field_ent[i+1];
		fields.insert(std::string(buffer + item->start, buffer + item->end));
	}

	std::set<std::uint32_t> torrent_ids;
	parse_ids(torrent_ids, args, buffer);

	std::vector<torrent_status> t;
	m_ses.get_torrent_status(&t, &all_torrents);

	appendf(buf, "{ \"result\": \"success\", \"arguments\": { \"torrents\": [");

#define TORRENT_PROPERTY(name, format_code, prop) \
	if (fields.count(name)) { \
		appendf(buf, ", \"" name "\": " format_code "" + (count?0:2), prop); \
		++count; \
	}

	int returned_torrents = 0;
	error_code ec;
	torrent_info empty("", ec);
	for (int i = 0; i < t.size(); ++i)
	{
		torrent_info const* ti = &empty;
		shared_ptr<torrent_info const> holder;
		if (t[i].has_metadata)
		{
			holder = t[i].torrent_file.lock();
			ti = holder.get();
		}
		torrent_status const& ts = t[i];

		if (!torrent_ids.empty() && torrent_ids.count(ts.handle.id()) == 0)
			continue;

		// skip comma on any item that's not the first one
		appendf(buf, ", {" + (returned_torrents?0:2));
		int count = 0;
		TORRENT_PROPERTY("activityDate", "%" PRId64, time(0) - (std::min)(ts.time_since_download
			, ts.time_since_upload));
		TORRENT_PROPERTY("addedDate", "%" PRId64, ts.added_time);
		TORRENT_PROPERTY("comment", "\"%s\"", escape_json(ti->comment()).c_str());
		TORRENT_PROPERTY("creator", "\"%s\"", escape_json(ti->creator()).c_str());
		TORRENT_PROPERTY("dateCreated", "%" PRId64, ti->creation_date() ? ti->creation_date().get() : 0);
		TORRENT_PROPERTY("doneDate", "%" PRId64, ts.completed_time);
		TORRENT_PROPERTY("downloadDir", "\"%s\"", escape_json(ts.save_path).c_str());
		TORRENT_PROPERTY("error", "%d", ts.errc ? 0 : 1);
		TORRENT_PROPERTY("errorString", "\"%s\"", escape_json(ts.errc.message()).c_str());
		TORRENT_PROPERTY("eta", "%d", ts.download_payload_rate <= 0 ? -1
			: (ts.total_wanted - ts.total_wanted_done) / ts.download_payload_rate);
		TORRENT_PROPERTY("hashString", "\"%s\"", to_hex(ts.handle.info_hash().to_string()).c_str());
		TORRENT_PROPERTY("downloadedEver", "%" PRId64, ts.all_time_download);
		TORRENT_PROPERTY("downloadLimit", "%d", ts.handle.download_limit());
		TORRENT_PROPERTY("downloadLimited", "%s", to_bool(ts.handle.download_limit() > 0));
		TORRENT_PROPERTY("haveValid", "%d", ts.num_pieces);
		TORRENT_PROPERTY("id", "%u", ts.handle.id());
		TORRENT_PROPERTY("isFinished", "%s", to_bool(ts.is_finished));
		TORRENT_PROPERTY("isPrivate", "%s", to_bool(ti->priv()));
		TORRENT_PROPERTY("isStalled", "%s", to_bool(ts.download_payload_rate == 0));
		TORRENT_PROPERTY("leftUntilDone", "%" PRId64, ts.total_wanted - ts.total_wanted_done);
		TORRENT_PROPERTY("magnetLink", "\"%s\"", ti == &empty ? "" : make_magnet_uri(*ti).c_str());
		TORRENT_PROPERTY("metadataPercentComplete", "%f", ts.has_metadata ? 1.f : ts.progress_ppm / 1000000.f);
		TORRENT_PROPERTY("name", "\"%s\"", escape_json(ts.name).c_str());
		TORRENT_PROPERTY("peer-limit", "%d", ts.handle.max_connections());
		TORRENT_PROPERTY("peersConnected", "%d", ts.num_peers);
		// even though this is called "percentDone", it's really expecting the
		// progress in the range [0, 1]
		TORRENT_PROPERTY("percentDone", "%f", ts.progress_ppm / 1000000.f);
		TORRENT_PROPERTY("pieceCount", "%d", ti != &empty ? ti->num_pieces() : 0);
		TORRENT_PROPERTY("pieceSize", "%d", ti != &empty ? ti->piece_length() : 0);
		TORRENT_PROPERTY("queuePosition", "%d", ts.queue_position);
		TORRENT_PROPERTY("rateDownload", "%d", ts.download_rate);
		TORRENT_PROPERTY("rateUpload", "%d", ts.upload_rate);
		TORRENT_PROPERTY("recheckProgress", "%f", ts.progress_ppm / 1000000.f);
		TORRENT_PROPERTY("secondsDownloading", "%" PRId64 , ts.active_time);
		TORRENT_PROPERTY("secondsSeeding", "%" PRId64, ts.finished_time);
		TORRENT_PROPERTY("sizeWhenDone", "%" PRId64, ti != &empty ? ti->total_size() : 0);
		TORRENT_PROPERTY("totalSize", "%" PRId64, ts.total_done);
		TORRENT_PROPERTY("uploadedEver", "%" PRId64, ts.all_time_upload);
		TORRENT_PROPERTY("uploadLimit", "%d", ts.handle.upload_limit());
		TORRENT_PROPERTY("uploadLimited", "%s", to_bool(ts.handle.upload_limit() > 0));
		TORRENT_PROPERTY("uploadedRatio", "%ld", ts.all_time_download == 0
			? -2 : ts.all_time_upload / ts.all_time_download);

		if (fields.count("status"))
		{
			appendf(buf, ", \"status\": %d" + (count?0:2), torrent_tr_status(ts));
			++count;
		}

		if (fields.count("files"))
		{
			file_storage const& files = ti->files();
			std::vector<std::int64_t> progress;
			ts.handle.file_progress(progress);
			appendf(buf, ", \"files\": [" + (count?0:2));
			for (int i = 0; i < files.num_files(); ++i)
			{
				appendf(buf, ", { \"bytesCompleted\": %" PRId64 ","
					"\"length\": %" PRId64 ","
					"\"name\": \"%s\" }" + (i?0:2)
					, progress[i], files.file_size(i), escape_json(files.file_path(i)).c_str());
			}
			appendf(buf, "]");
			++count;
		}

		if (fields.count("fileStats"))
		{
			file_storage const& files = ti->files();
			std::vector<std::int64_t> progress;
			ts.handle.file_progress(progress);
			appendf(buf, ", \"fileStats\": [" + (count?0:2));
			for (int i = 0; i < files.num_files(); ++i)
			{
				int prio = ts.handle.file_priority(i);
				appendf(buf, ", { \"bytesCompleted\": %" PRId64 ","
					"\"wanted\": %s,"
					"\"priority\": %d }" + (i?0:2)
					, progress[i], to_bool(prio), tr_file_priority(prio));
			}
			appendf(buf, "]");
			++count;
		}

		if (fields.count("wanted"))
		{
			file_storage const& files = ti->files();
			appendf(buf, ", \"wanted\": [" + (count?0:2));
			for (int i = 0; i < files.num_files(); ++i)
			{
				appendf(buf, ", %s" + (i?0:2)
					, to_bool(ts.handle.file_priority(i)));
			}
			appendf(buf, "]");
			++count;
		}

		if (fields.count("priorities"))
		{
			file_storage const& files = ti->files();
			appendf(buf, ", \"priorities\": [" + (count?0:2));
			for (int i = 0; i < files.num_files(); ++i)
			{
				appendf(buf, ", %d" + (i?0:2)
					, tr_file_priority(ts.handle.file_priority(i)));
			}
			appendf(buf, "]");
			++count;
		}

		if (fields.count("webseeds"))
		{
			std::vector<web_seed_entry> const& webseeds = ti->web_seeds();
			appendf(buf, ", \"webseeds\": [" + (count?0:2));
			for (int i = 0; i < webseeds.size(); ++i)
			{
				appendf(buf, ", \"%s\"" + (i?0:2)
					, escape_json(webseeds[i].url).c_str());
			}
			appendf(buf, "]");
			++count;
		}

		if (fields.count("pieces"))
		{
			std::string encoded_pieces = base64encode(
				std::string(ts.pieces.data(), (ts.pieces.size() + 7) / 8));
			appendf(buf, ", \"pieces\": \"%s\"" + (count?0:2)
				, encoded_pieces.c_str());
			++count;
		}

		if (fields.count("peers"))
		{
			std::vector<peer_info> peers;
			ts.handle.get_peer_info(peers);
			appendf(buf, ", \"peers\": [" + (count?0:2));
			for (int i = 0; i < peers.size(); ++i)
			{
				peer_info const& p = peers[i];
				appendf(buf, ", { \"address\": \"%s\""
					", \"clientName\": \"%s\""
					", \"clientIsChoked\": %s"
					", \"clientIsInterested\": %s"
					", \"flagStr\": \"\""
					", \"isDownloadingFrom\": %s"
					", \"isEncrypted\": %s"
					", \"isIncoming\": %s"
					", \"isUploadingTo\": %s"
					", \"isUTP\": %s"
					", \"peerIsChoked\": %s"
					", \"peerIsInterested\": %s"
					", \"port\": %d"
					", \"progress\": %f"
					", \"rateToClient\": %d"
					", \"rateToPeer\": %d"
					"}"
					+ (i?0:2)
					, print_address(p.ip.address()).c_str()
					, escape_json(p.client).c_str()
					, to_bool(p.flags & peer_info::choked)
					, to_bool(p.flags & peer_info::interesting)
					, to_bool(p.downloading_piece_index != -1)
					, to_bool(p.flags & (peer_info::rc4_encrypted | peer_info::plaintext_encrypted))
					, to_bool(p.source & peer_info::incoming)
					, to_bool(p.used_send_buffer)
					, to_bool(p.flags & peer_info::utp_socket)
					, to_bool(p.flags & peer_info::remote_choked)
					, to_bool(p.flags & peer_info::remote_interested)
					, p.ip.port()
					, p.progress
					, p.down_speed
					, p.up_speed
					);
			}
			appendf(buf, "]");
			++count;
		}

		if (fields.count("trackers"))
		{
			std::vector<announce_entry> trackers = ts.handle.trackers();
			appendf(buf, ", \"trackers\": [" + (count?0:2));
			for (int i = 0; i < trackers.size(); ++i)
			{
				announce_entry const& a = trackers[i];
				appendf(buf, ", { \"announce\": \"%s\""
					", \"id\": %u"
					", \"scrape\": \"%s\""
					", \"tier\": %d"
					"}"
					+ (i?0:2)
					, a.url.c_str(), tracker_id(a), a.url.c_str(), a.tier);
			}
			appendf(buf, "]");
			++count;
		}

		if (fields.count("trackerStats"))
		{
			std::vector<announce_entry> trackers = ts.handle.trackers();
			appendf(buf, ", \"trackerStats\": [" + (count?0:2));
			for (int i = 0; i < trackers.size(); ++i)
			{
				announce_entry const& a = trackers[i];
				using boost::tuples::ignore;
				error_code ec;
				std::string hostname;
				boost::tie(ignore, ignore, hostname, ignore, ignore)
					= parse_url_components(a.url, ec);
				appendf(buf, ", { \"announce\": \"%s\""
					", \"announceState\": %u"
					", \"downloadCount\": %d"
					", \"hasAnnounced\": %s"
					", \"hasScraped\": %s"
					", \"host\": \"%s\""
					", \"id\": %u"
					", \"isBackup\": %s"
					", \"lastAnnouncePeerCount\": %d"
					", \"lastAnnounceResult\": \"%s\""
					", \"lastAnnounceStartTime\": %" PRId64
					", \"lastAnnounceSucceeded\": %" PRId64
					", \"lastAnnounceTime\": %" PRId64
					", \"lastAnnounceTimeOut\": %s"
					", \"lastScrapePeerCount\": %d"
					", \"lastScrapeResult\": \"%s\""
					", \"lastScrapeStartTime\": %" PRId64
					", \"lastScrapeSucceeded\": %" PRId64
					", \"lastScrapeTime\": %" PRId64
					", \"lastScrapeTimeOut\": %s"
					", \"leecherCount\": %d"
					", \"nextAnnounceTime\": %" PRId64
					", \"nextScrapeTime\": %" PRId64
					", \"scrape\": \"%s\""
					", \"scrapeState\": %d"
					", \"seederCount\": %d"
					", \"tier\": %d"
					"}"
					+ (i?0:2)
					, escape_json(a.url).c_str()
					, tracker_status(a, ts)
					, 0
					, to_bool(a.start_sent)
					, to_bool(false)
					, hostname.c_str()
					, tracker_id(a)
					, to_bool(false)
					, 0 // lastAnnouncePeerCount
					, a.last_error.message().c_str() // lastAnnounceResult
					, 0 // lastAnnounceStartTime
					, to_bool(!a.last_error) // lastAnnounceSucceeded
					, 0 // lastAnnounceTime
					, to_bool(a.last_error == boost::asio::error::timed_out) // lastAnnounceTimeOut
					, 0, "", 0, "false", 0, "false"
					, 0 // leecherCount
					, time(NULL) + a.next_announce_in()
					, 0
					, a.url.c_str()
					, 0
					, 0 // seederCount
					, a.tier);
			}
			appendf(buf, "]");
			++count;
		}
		appendf(buf, "}");
		++returned_torrents;
	}

	appendf(buf, "] }, \"tag\": %" PRId64 " }", tag);
}

void transmission_webui::set_torrent(std::vector<char>& buf, jsmntok_t* args
	, std::int64_t tag, char* buffer, permissions_interface const* p)
{
	if (!p->allow_set_settings(-1))
	{
		return_failure(buf, "permission denied", tag);
		return;
	}

	std::vector<torrent_handle> handles;
	get_torrents(handles, args, buffer);

	bool set_dl_limit = false;
	int download_limit = find_int(args, buffer, "downloadLimit", &set_dl_limit);
	bool download_limited = find_bool(args, buffer, "downloadLimited");
	if (!download_limited) download_limit = 0;

	bool set_ul_limit = false;
	int upload_limit = find_int(args, buffer, "uploadLimit", &set_ul_limit);
	bool upload_limited = find_bool(args, buffer, "uploadLimited");
	if (!upload_limited) upload_limit = 0;

	bool move_storage = false;
	std::string location = find_string(args, buffer, "location", &move_storage);

	bool set_max_conns = false;
	int max_connections = find_int(args, buffer, "peer-limit", &set_max_conns);

	std::vector<announce_entry> add_trackers;
	jsmntok_t* tracker_add = find_key(args, buffer, "trackerAdd", JSMN_ARRAY);
	if (tracker_add)
	{
		jsmntok_t* item = tracker_add + 1;
		for (int i = 0; i < tracker_add->size; ++i, item = skip_item(item))
		{
			if (item->type != JSMN_STRING) continue;
			add_trackers.push_back(announce_entry(std::string(
				buffer + item->start, item->end - item->start)));
		}
	}

	int all_file_prio = -1;
	std::vector<std::pair<int, int> > file_priority;

	jsmntok_t* file_prio_unwanted = find_key(args, buffer, "files-unwanted", JSMN_ARRAY);
	if (file_prio_unwanted)
	{
		if (file_prio_unwanted->size == 0) all_file_prio = 0;
		jsmntok_t* item = file_prio_unwanted + 1;
		for (int i = 0; i < file_prio_unwanted->size; ++i, item = skip_item(item))
		{
			if (item->type != JSMN_PRIMITIVE) continue;
			int index = atoi(buffer + item->start);
			file_priority.push_back(std::make_pair(index, 0));
		}
	}

	jsmntok_t* file_prio_wanted = find_key(args, buffer, "files-wanted", JSMN_ARRAY);
	if (file_prio_wanted)
	{
		if (file_prio_wanted->size == 0) all_file_prio = 2;
		jsmntok_t* item = file_prio_wanted + 1;
		for (int i = 0; i < file_prio_wanted->size; ++i, item = skip_item(item))
		{
			if (item->type != JSMN_PRIMITIVE) continue;
			int index = atoi(buffer + item->start);
			file_priority.push_back(std::make_pair(index, 2));
		}
	}

	jsmntok_t* file_prio_high = find_key(args, buffer, "priority-high", JSMN_ARRAY);
	if (file_prio_high)
	{
		if (file_prio_high->size == 0) all_file_prio = 7;
		jsmntok_t* item = file_prio_high + 1;
		for (int i = 0; i < file_prio_high->size; ++i, item = skip_item(item))
		{
			if (item->type != JSMN_PRIMITIVE) continue;
			int index = atoi(buffer + item->start);
			file_priority.push_back(std::make_pair(index, 7));
		}
	}

	jsmntok_t* file_prio_low = find_key(args, buffer, "priority-low", JSMN_ARRAY);
	if (file_prio_low)
	{
		if (file_prio_low->size == 0) all_file_prio = 1;
		jsmntok_t* item = file_prio_low + 1;
		for (int i = 0; i < file_prio_low->size; ++i, item = skip_item(item))
		{
			if (item->type != JSMN_PRIMITIVE) continue;
			int index = atoi(buffer + item->start);
			file_priority.push_back(std::make_pair(index, 1));
		}
	}

	jsmntok_t* file_prio_normal = find_key(args, buffer, "priority-normal", JSMN_ARRAY);
	if (file_prio_normal)
	{
		if (file_prio_normal->size == 0) all_file_prio = 2;
		jsmntok_t* item = file_prio_normal + 1;
		for (int i = 0; i < file_prio_normal->size; ++i, item = skip_item(item))
		{
			if (item->type != JSMN_PRIMITIVE) continue;
			int index = atoi(buffer + item->start);
			file_priority.push_back(std::make_pair(index, 2));
		}
	}

	for (std::vector<torrent_handle>::iterator i = handles.begin()
		, end(handles.end()); i != end; ++i)
	{
		torrent_handle& h = *i;

		if (set_dl_limit) h.set_download_limit(download_limit * 1000);
		if (set_ul_limit) h.set_upload_limit(upload_limit * 1000);
		if (move_storage) h.move_storage(location);
		if (set_max_conns) h.set_max_connections(max_connections);
		if (!add_trackers.empty())
		{
			std::vector<announce_entry> trackers =  h.trackers();
			trackers.insert(trackers.end(), add_trackers.begin(), add_trackers.end());
			h.replace_trackers(trackers);
		}
		if (!file_priority.empty())
		{
			std::vector<int> prio = h.file_priorities();
			if (all_file_prio != -1) std::fill(prio.begin(), prio.end(), all_file_prio);
			for (std::vector<std::pair<int, int> >::iterator
				i = file_priority.begin(), end(file_priority.end());
				i != end; ++i)
			{
				if (i->first < 0 || i->first >= prio.size()) continue;
				prio[i->first] = i->second;
			}
			h.prioritize_files(prio);
		}
	}
}

void transmission_webui::start_torrent(std::vector<char>& buf, jsmntok_t* args
	, std::int64_t tag, char* buffer, permissions_interface const* p)
{
	if (!p->allow_start())
	{
		return_failure(buf, "permission denied", tag);
		return;
	}

	std::vector<torrent_handle> handles;
	get_torrents(handles, args, buffer);
	for (std::vector<torrent_handle>::iterator i = handles.begin()
		, end(handles.end()); i != end; ++i)
	{
		i->auto_managed(true);
		i->resume();
	}
	appendf(buf, "{ \"result\": \"success\", \"tag\": %" PRId64 ", "
		"\"arguments\": {} }", tag);
}

void transmission_webui::start_torrent_now(std::vector<char>& buf, jsmntok_t* args
	, std::int64_t tag, char* buffer, permissions_interface const* p)
{
	if (!p->allow_start())
	{
		return_failure(buf, "permission denied", tag);
		return;
	}

	std::vector<torrent_handle> handles;
	get_torrents(handles, args, buffer);
	for (std::vector<torrent_handle>::iterator i = handles.begin()
		, end(handles.end()); i != end; ++i)
	{
		i->auto_managed(false);
		i->resume();
	}
	appendf(buf, "{ \"result\": \"success\", \"tag\": %" PRId64 ", "
		"\"arguments\": {} }", tag);
}

void transmission_webui::stop_torrent(std::vector<char>& buf, jsmntok_t* args
	, std::int64_t tag, char* buffer, permissions_interface const* p)
{
	if (!p->allow_stop())
	{
		return_failure(buf, "permission denied", tag);
		return;
	}

	std::vector<torrent_handle> handles;
	get_torrents(handles, args, buffer);
	for (std::vector<torrent_handle>::iterator i = handles.begin()
		, end(handles.end()); i != end; ++i)
	{
		i->auto_managed(false);
		i->pause();
	}
	appendf(buf, "{ \"result\": \"success\", \"tag\": %" PRId64 ", "
		"\"arguments\": {} }", tag);
}

void transmission_webui::verify_torrent(std::vector<char>& buf, jsmntok_t* args
	, std::int64_t tag, char* buffer, permissions_interface const* p)
{
	if (!p->allow_recheck())
	{
		return_failure(buf, "permission denied", tag);
		return;
	}

	std::vector<torrent_handle> handles;
	get_torrents(handles, args, buffer);
	for (std::vector<torrent_handle>::iterator i = handles.begin()
		, end(handles.end()); i != end; ++i)
	{
		i->force_recheck();
	}
	appendf(buf, "{ \"result\": \"success\", \"tag\": %" PRId64 ", "
		"\"arguments\": {} }", tag);
}

void transmission_webui::reannounce_torrent(std::vector<char>& buf, jsmntok_t* args
	, std::int64_t tag, char* buffer, permissions_interface const* p)
{
	if (!p->allow_start())
	{
		return_failure(buf, "permission denied", tag);
		return;
	}

	std::vector<torrent_handle> handles;
	get_torrents(handles, args, buffer);
	for (std::vector<torrent_handle>::iterator i = handles.begin()
		, end(handles.end()); i != end; ++i)
	{
		i->force_reannounce();
	}
	appendf(buf, "{ \"result\": \"success\", \"tag\": %" PRId64 ", "
		"\"arguments\": {} }", tag);
}

void transmission_webui::remove_torrent(std::vector<char>& buf, jsmntok_t* args
	, std::int64_t tag, char* buffer, permissions_interface const* p)
{
	if (!p->allow_remove())
	{
		return_failure(buf, "permission denied", tag);
		return;
	}

	bool delete_data = find_bool(args, buffer, "delete-local-data");

	std::vector<torrent_handle> handles;
	get_torrents(handles, args, buffer);
	for (std::vector<torrent_handle>::iterator i = handles.begin()
		, end(handles.end()); i != end; ++i)
	{
		m_ses.remove_torrent(*i, delete_data ? session::delete_files : 0);
	}
	appendf(buf, "{ \"result\": \"success\", \"tag\": %" PRId64 ", "
		"\"arguments\": {} }", tag);
}

void transmission_webui::session_stats(std::vector<char>& buf, jsmntok_t* args
	, std::int64_t tag, char* buffer, permissions_interface const* p)
{
	if (!p->allow_session_status())
	{
		return_failure(buf, "permission denied", tag);
		return;
	}

	// TODO: post session stats instead, and capture the performance counters
	session_status st = m_ses.status();

	appendf(buf, "{ \"result\": \"success\", \"tag\": %" PRId64 ", "
		"\"arguments\": { "
		"\"activeTorrentCount\": %d,"
		"\"downloadSpeed\": %d,"
		"\"pausedTorrentCount\": %d,"
		"\"torrentCount\": %d,"
		"\"uploadSpeed\": %d,"
		"\"cumulative-stats\": {"
			"\"uploadedBytes\": %" PRId64 ","
			"\"downloadedBytes\": %" PRId64 ","
			"\"filesAdded\": %d,"
			"\"sessionCount\": %d,"
			"\"secondsActive\": %d"
			"},"
		"\"current-stats\": {"
			"\"uploadedBytes\": %" PRId64 ","
			"\"downloadedBytes\": %" PRId64 ","
			"\"filesAdded\": %d,"
			"\"sessionCount\": %d,"
			"\"secondsActive\": %d"
			"}"
		"}}", tag
		, st.num_torrents - st.num_paused_torrents
		, st.payload_download_rate
		, st.num_paused_torrents
		, st.num_torrents
		, st.payload_upload_rate
		// cumulative-stats (not supported)
		, st.total_payload_upload
		, st.total_payload_download
		, st.num_torrents
		, 1
		, time(nullptr) - m_start_time
		// current-stats
		, st.total_payload_upload
		, st.total_payload_download
		, st.num_torrents
		, 1
		, time(nullptr) - m_start_time);
}

void transmission_webui::get_session(std::vector<char>& buf, jsmntok_t* args
	, std::int64_t tag, char* buffer, permissions_interface const* p)
{
	if (!p->allow_get_settings(-1))
	{
		return_failure(buf, "permission denied", tag);
		return;
	}

	session_status st = m_ses.status();
	settings_pack sett = m_ses.get_settings();

	pe_settings pes = m_ses.get_pe_settings();
	appendf(buf, "{ \"result\": \"success\", \"tag\": %" PRId64 ", "
		"\"arguments\": { "
		"\"alt-speed-down\": 0,"
		"\"alt-speed-enabled\": false,"
		"\"alt-speed-time-begin\": 0,"
		"\"alt-speed-time-enabled\": false,"
		"\"alt-speed-time-end\": 0,"
		"\"alt-speed-time-day\": 0,"
		"\"alt-speed-up\": 0,"
		"\"blocklist-url\": \"\","
		"\"blocklist-enabled\": false,"
		"\"blocklist-size\": 0,"
		"\"cache-size-mb\": %d,"
		"\"config-dir\": \"\","
		"\"download-dir\": \"%s\","
		"\"download-dir-free-space\": %" PRId64 ","
		"\"download-queue-size\": %d,"
		"\"download-queue-enabled\": true,"
		"\"seed-queue-size\": %d,"
		"\"seed-queue-enabled\": true,"
		"\"speed-limit-down\": %d,"
		"\"speed-limit-up\": %d,"
		"\"speed-limit-down-enabled\": %s,"
		"\"speed-limit-up-enabled\": %s,"
		"\"start-added-torrents\": %s,"
		"\"units\": { "
			"\"speed-units\": [\"kB/s\", \"MB/s\", \"GB/s\", \"TB/s\"],"
			"\"speed-bytes\": [1000, 1000000, 1000000000, 1000000000000],"
			"\"size-units\": [\"kB\", \"MB\", \"GB\", \"TB\"],"
			"\"size-bytes\": [1000, 1000000, 1000000000, 1000000000000],"
			"\"memory-units\": [\"kB\", \"MB\", \"GB\", \"TB\"],"
			"\"memory-bytes\": [1000, 1000000, 1000000000, 1000000000000]"
			"},"
		"\"utp-enabled\": %s,"
		"\"version\": \"%s\","
		"\"peer-port\": %d,"
		"\"peer-limit-global\": %d,"
		"\"encryption\": \"%s\""
		"}}",tag
		, sett.get_int(settings_pack::cache_size) * 16 / 1024
		, m_params_model.save_path.c_str()
		, free_disk_space(m_params_model.save_path)
		, sett.get_int(settings_pack::active_downloads)
		, sett.get_int(settings_pack::active_seeds)
		, sett.get_int(settings_pack::download_rate_limit) / 1000
		, sett.get_int(settings_pack::upload_rate_limit) / 1000
		, to_bool(sett.get_int(settings_pack::download_rate_limit) > 0)
		, to_bool(sett.get_int(settings_pack::upload_rate_limit) > 0)
		, to_bool((m_params_model.flags & add_torrent_params::flag_auto_managed)
			|| (m_params_model.flags & add_torrent_params::flag_paused) == 0)
		, to_bool(sett.get_bool(settings_pack::enable_incoming_utp)
			|| sett.get_bool(settings_pack::enable_outgoing_utp))
		, sett.get_str(settings_pack::user_agent).c_str()
		, m_ses.listen_port()
		, sett.get_int(settings_pack::connections_limit)
		, pes.in_enc_policy == pe_settings::forced ? "required"
			: pes.prefer_rc4 ? "preferred" : "tolerated"
	);
}

void transmission_webui::set_session(std::vector<char>& buf, jsmntok_t* args, std::int64_t tag
	, char* buffer, permissions_interface const* p)
{
	settings_pack pack;

	int num_keys = args->size / 2;
	for (jsmntok_t* i = args+1; num_keys > 0; i = skip_item(skip_item(i)), --num_keys)
	{
		if (i->type != JSMN_STRING) continue;
		buffer[i->end] = 0;
		buffer[i[1].end] = 0;
		char const* key = &buffer[i->start];
		char const* value = &buffer[i[1].start];

		if (strcmp(key, "alt-speed-down") == 0)
		{
			
		}
/*		else if (strcmp(key, "alt-speed-enabled") == 0)
		{
			
		}
		else if (strcmp(key, "alt-speed-time-begin") == 0)
		{
			
		}
		else if (strcmp(key, "alt-speed-time-enabled") == 0)
		{
			
		}
		else if (strcmp(key, "alt-speed-time-end") == 0)
		{
			
		}
		else if (strcmp(key, "alt-speed-time-day") == 0)
		{
			
		}
		else if (strcmp(key, "alt-speed-up") == 0)
		{
			
		}
		else if (strcmp(key, "blocklist-url") == 0)
		{
			
		}
		else if (strcmp(key, "blocklist-enabled") == 0)
		{
			
		}
		else if (strcmp(key, "blocklist-size") == 0)
		{
			
		}
*/		else if (strcmp(key, "cache-size-mb") == 0)
		{
			if (!p->allow_set_settings(settings_pack::cache_size)) continue;
			int val = atoi(value);
			// convert Megabytes to 16 kiB blocks
			pack.set_int(settings_pack::cache_size, val * 1024 / 16);
		}
/*		else if (strcmp(key, "config-dir") == 0)
		{
			
		}
*/		else if (strcmp(key, "download-dir") == 0)
		{
			if (!p->allow_set_settings(-1)) continue;
			m_params_model.save_path = value;
			if (m_settings) m_settings->set_str("save_path", value);
		}
		else if (strcmp(key, "download-queue-size") == 0)
		{
			if (!p->allow_set_settings(settings_pack::active_downloads)) continue;
			int val = atoi(value);
			pack.set_int(settings_pack::active_downloads, val);
		}
/*		else if (strcmp(key, "download-queue-enabled") == 0)
		{
			
		}
*/		else if (strcmp(key, "seed-queue-size") == 0)
		{
			if (!p->allow_set_settings(settings_pack::active_seeds)) continue;
			int val = atoi(value);
			pack.set_int(settings_pack::active_seeds, val);
		}
/*		else if (strcmp(key, "seed-queue-enabled") == 0)
		{
			
		}
*/		else if (strcmp(key, "speed-limit-down") == 0)
		{
			if (!p->allow_set_settings(settings_pack::download_rate_limit)) continue;
			int val = atoi(value) * 1000;
			pack.set_int(settings_pack::download_rate_limit, val);
		}
		else if (strcmp(key, "speed-limit-up") == 0)
		{
			if (!p->allow_set_settings(settings_pack::upload_rate_limit)) continue;
			int val = atoi(value) * 1000;
			pack.set_int(settings_pack::upload_rate_limit, val);
		}
		else if (strcmp(key, "speed-limit-down-enabled") == 0)
		{
			if (!p->allow_set_settings(settings_pack::download_rate_limit)) continue;
			if (strcmp(value, "true") == 0)
			{
				// libtorrent uses a single value to specify the rate limit
				// including the case where it's disabled. There's no
				// trivial way to remember the rate when disabling it
				pack.set_int(settings_pack::download_rate_limit, 100000);
			}
			else
			{
				pack.set_int(settings_pack::download_rate_limit, 0);
			}
		}
		else if (strcmp(key, "speed-limit-up-enabled") == 0)
		{
			if (!p->allow_set_settings(settings_pack::upload_rate_limit)) continue;
			if (strcmp(value, "true") == 0)
			{
				// libtorrent uses a single value to specify the rate limit
				// including the case where it's disabled. There's no
				// trivial way to remember the rate when disabling it
				pack.set_int(settings_pack::upload_rate_limit, 100000);
			}
			else
			{
				pack.set_int(settings_pack::upload_rate_limit, 0);
			}
		}
		else if (strcmp(key, "start-added-torrents") == 0)
		{
			if (!p->allow_set_settings(-1)) continue;
			bool start = strcmp(value, "true") == 0;
			m_params_model.flags |= start ? 0 : add_torrent_params::flag_paused;
			m_params_model.flags &= ~(start ? 0 : add_torrent_params::flag_auto_managed);
		}
		else if (strcmp(key, "peer-port") == 0)
		{
			if (!p->allow_set_settings(-1)) continue;
			int port = atoi(value);
			error_code ec;
			m_ses.listen_on(std::make_pair(port, port+1), ec);
			if (m_settings) m_settings->set_int("listen_port", port);
		}
		else if (strcmp(key, "utp-enabled") == 0)
		{
			if (!p->allow_set_settings(settings_pack::enable_outgoing_utp)
				|| !p->allow_set_settings(settings_pack::enable_outgoing_utp)) continue;
			bool utp = strcmp(value, "true") == 0;
			pack.set_bool(settings_pack::enable_outgoing_utp, utp);
			pack.set_bool(settings_pack::enable_incoming_utp, utp);
		}
		else if (strcmp(key, "peer-limit-global") == 0)
		{
			if (!p->allow_set_settings(settings_pack::connections_limit)) continue;
			int num = atoi(value);
			pack.set_int(settings_pack::connections_limit, num);
		}
		else if (strcmp(key, "encryption") == 0)
		{
			if (!p->allow_set_settings(-1)) continue;
			pe_settings pes = m_ses.get_pe_settings();
			if (strcmp(value, "required") == 0)
			{
				pes.in_enc_policy = pe_settings::forced;
				pes.out_enc_policy = pe_settings::forced;
				pes.allowed_enc_level = pe_settings::rc4;
				pes.prefer_rc4 = true;
			}
			else if (strcmp(value, "preferred") == 0)
			{
				pes.in_enc_policy = pe_settings::enabled;
				pes.out_enc_policy = pe_settings::enabled;
				pes.allowed_enc_level = pe_settings::both;
				pes.prefer_rc4 = true;
			}
			else
			{
				// tolerated
				pes.in_enc_policy = pe_settings::enabled;
				pes.out_enc_policy = pe_settings::enabled;
				pes.allowed_enc_level = pe_settings::both;
				pes.prefer_rc4 = false;
			}
		}
/*		else if (strcmp(value, "trash-original-torrent-files") == 0)
		{
		}
*/		else
		{
			fprintf(stderr, "UNHANDLED SETTING: %s: %s\n", key, value);
		}
	}

	m_ses.apply_settings(pack);

	if (m_settings)
	{
		error_code ec;
		m_settings->save(ec);
	}
}

void transmission_webui::get_torrents(std::vector<torrent_handle>& handles, jsmntok_t* args
	, char* buffer)
{
	std::vector<torrent_handle> h = m_ses.get_torrents();

	std::set<std::uint32_t> torrent_ids;
	parse_ids(torrent_ids, args, buffer);

	if (torrent_ids.empty())
	{
		// if ids is omitted, return all torrents
		handles.swap(h);
		return;
	}
	for (std::vector<torrent_handle>::iterator i = h.begin()
		, end(h.end()); i != end; ++i)
	{
		if (torrent_ids.count(i->id()))
			handles.insert(handles.begin(), *i);
	}
}

transmission_webui::transmission_webui(session& s, save_settings_interface* sett, auth_interface const* auth)
	: m_ses(s)
	, m_settings(sett)
	, m_auth(auth)
{
	if (m_auth == NULL)
	{
		const static no_auth n;
		m_auth = &n;
	}

	m_params_model.save_path = ".";
	m_start_time = time(NULL);

	if (m_settings)
	{
		m_params_model.save_path = m_settings->get_str("save_path", ".");
		int port = m_settings->get_int("listen_port", -1);
		if (port != -1)
		{
			error_code ec;
			m_ses.listen_on(std::make_pair(port, port+1), ec);
		}
	}
}

transmission_webui::~transmission_webui() {}

bool transmission_webui::handle_http(mg_connection* conn, mg_request_info const* request_info)
{
	// we only provide access to paths under /web and /upload
	if (strcmp(request_info->uri, "/transmission/rpc")
		&& strcmp(request_info->uri, "/rpc")
		&& strcmp(request_info->uri, "/upload"))
		return false;

	permissions_interface const* perms = parse_http_auth(conn, m_auth);
	if (perms == NULL)
	{
		mg_printf(conn, "HTTP/1.1 401 Unauthorized\r\n"
			"WWW-Authenticate: Basic realm=\"BitTorrent\"\r\n"
			"Content-Length: 0\r\n\r\n");
		return true;
	}

	if (strcmp(request_info->uri, "/upload") == 0)
	{
		if (!perms->allow_add())
		{
			mg_printf(conn, "HTTP/1.1 401 Unauthorized\r\n"
				"WWW-Authenticate: Basic realm=\"BitTorrent\"\r\n"
				"Content-Length: 0\r\n\r\n");
			return true;
		}
		add_torrent_params p = m_params_model;
		error_code ec;
		if (!parse_torrent_post(conn, p, ec))
		{
			mg_printf(conn, "HTTP/1.1 400 Invalid Request\r\n"
				"Connection: close\r\n\r\n");
			return true;
		}

		char buf[10];
		if (mg_get_var(request_info->query_string, strlen(request_info->query_string)
			, "paused", buf, sizeof(buf)) > 0
			&& strcmp(buf, "true") == 0)
		{
			p.flags |= add_torrent_params::flag_paused;
			p.flags &= ~add_torrent_params::flag_auto_managed;
		}

		m_ses.async_add_torrent(p);

		mg_printf(conn, "HTTP/1.1 200 OK\r\n"
			"Content-Type: text/json\r\n"
			"Content-Length: 0\r\n\r\n");
		return true;
	}

	char const* cl = mg_get_header(conn, "content-length");
	std::vector<char> post_body;
	if (cl != NULL)
	{
		int content_length = atoi(cl);
		if (content_length > 0 && content_length < 10 * 1024 * 1024)
		{
			post_body.resize(content_length + 1);
			mg_read(conn, &post_body[0], post_body.size());
			// null terminate
			post_body[content_length] = 0;
		}
	}

//	printf("REQUEST: %s%s%s\n", request_info->uri
//		, request_info->query_string ? "?" : ""
//		, request_info->query_string ? request_info->query_string : "");

	std::vector<char> response;
	if (post_body.empty())
	{
		return_error(conn, "request with no POST body");
		return true;
	}
	jsmntok_t tokens[256];
	jsmn_parser p;
	jsmn_init(&p);

	int r = jsmn_parse(&p, &post_body[0], tokens, sizeof(tokens)/sizeof(tokens[0]));
	if (r == JSMN_ERROR_INVAL)
	{
		return_error(conn, "request not JSON");
		return true;
	}
	else if (r == JSMN_ERROR_NOMEM)
	{
		return_error(conn, "request too big");
		return true;
	}
	else if (r == JSMN_ERROR_PART)
	{
		return_error(conn, "request truncated");
		return true;
	}
	else if (r != JSMN_SUCCESS)
	{
		return_error(conn, "invalid request");
		return true;
	}

	handle_json_rpc(response, tokens, &post_body[0], perms);

	// we need a null terminator
	response.push_back('\0');
	// subtract one from content-length
	// to not count null terminator
	mg_printf(conn, "HTTP/1.1 200 OK\r\n"
		"Content-Type: text/json\r\n"
		"Content-Length: %d\r\n\r\n", int(response.size()) - 1);
	mg_write(conn, &response[0], response.size());
//	printf("%s\n", &response[0]);
	return true;
}


}

