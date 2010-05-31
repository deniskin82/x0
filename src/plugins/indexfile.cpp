/* <x0/mod_indexfile.cpp>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 *
 * (c) 2009 Chrisitan Parpart <trapni@gentoo.org>
 */

#include <x0/http/plugin.hpp>
#include <x0/http/server.hpp>
#include <x0/http/request.hpp>
#include <x0/http/response.hpp>
#include <x0/http/header.hpp>
#include <x0/strutils.hpp>
#include <x0/types.hpp>
#include <cstring>
#include <cerrno>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

/**
 * \ingroup plugins
 * \brief implements automatic index file resolving, if mapped request path is a path.
 */
class indexfile_plugin :
	public x0::plugin
{
private:
	x0::server::request_parse_hook::connection c;

	struct context : public x0::scope_value
	{
		std::vector<std::string> index_files;

		virtual void merge(const x0::scope_value *value)
		{
			if (auto cx = dynamic_cast<const context *>(value))
			{
				if (index_files.empty())
					index_files = cx->index_files;
			}
		}
	};

public:
	indexfile_plugin(x0::server& srv, const std::string& name) :
		x0::plugin(srv, name)
	{
		// to connect to resolved_entity at slot-group `1`, so, that all other transforms have taken place already,
		// that is, e.g. "userdir".
		// XXX a better implementation of this dependency-issue surely is, to introduce
		// another signal that would order the event sequence for us, but i'm not yet that clear about how
		// to name this in a clean and reasonable way.
		using namespace std::placeholders;
		c = server_.resolve_entity.connect(/*FIXME 1, */ std::bind(&indexfile_plugin::indexfile, this, _1));

		server_.register_cvar("IndexFiles", x0::context::server | x0::context::vhost, std::bind(&indexfile_plugin::setup_indexfiles, this, _1, _2));
	}

	~indexfile_plugin()
	{
		server_.resolve_entity.disconnect(c);
		server_.release(this);
	}

	void setup_indexfiles_srv(const x0::settings_value& cvar)
	{
		cvar.load(server_.acquire<context>(this)->index_files);
	}

	bool setup_indexfiles(const x0::settings_value& cvar, x0::scope& s)
	{
		return cvar.load(s.acquire<context>(this)->index_files);
	}

private:
	void indexfile(x0::request *in)
	{
		if (!in->fileinfo->is_directory())
			return;

		context *ctx = server_.vhost(in->hostid()).get<context>(this);
		if (!ctx)
			return;

		std::string path(in->fileinfo->filename());

		for (auto i = ctx->index_files.begin(), e = ctx->index_files.end(); i != e; ++i)
		{
			std::string ipath;
			ipath.reserve(path.length() + 1 + i->length());
			ipath += path;
			if (path[path.size() - 1] != '/')
				ipath += "/";
			ipath += *i;

			if (x0::fileinfo_ptr fi = in->connection.server().fileinfo(ipath))
			{
				if (fi->is_regular())
				{
					in->fileinfo = fi;
					break;
				}
			}
		}
	}
};

X0_EXPORT_PLUGIN(indexfile);
