/* <x0/mod_indexfile.cpp>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 *
 * (c) 2009 Chrisitan Parpart <trapni@gentoo.org>
 */

#include <x0/server.hpp>
#include <x0/request.hpp>
#include <x0/response.hpp>
#include <x0/header.hpp>
#include <x0/strutils.hpp>
#include <x0/types.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/bind.hpp>
#include <cstring>
#include <cerrno>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

/**
 * \ingroup modules
 * \brief implements automatic index file resolving, if mapped request path is a path.
 */
class indexfile_plugin :
	public x0::plugin
{
private:
	boost::signals::connection c;

	struct context
	{
		std::vector<std::string> index_files;
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
		c = server_.resolve_entity.connect(1, boost::bind(&indexfile_plugin::indexfile, this, _1));
		server_.create_context<context>(this);
	}

	~indexfile_plugin()
	{
		server_.resolve_entity.disconnect(c);
		server_.free_context<context>(this);
	}

	virtual void configure()
	{
		context& ctx = server_.context<context>(this);
		server_.config().load("IndexFiles", ctx.index_files);

		if (ctx.index_files.empty())
		{
			server_.log(x0::severity::warn, "indexfile module loaded, but no(/empty) configuration given.");
		}
	}

private:
	void indexfile(x0::request& in)
	{
		if (!in.fileinfo->is_directory())
			return;

		context& ctx = server_.context<context>(this);

		std::string path(in.fileinfo->filename());
		for (std::vector<std::string>::iterator i = ctx.index_files.begin(), e = ctx.index_files.end(); i != e; ++i)
		{
			std::string ipath;
			ipath.reserve(path.length() + 1 + i->length());
			ipath += path;
			if (path[path.size() - 1] != '/')
				ipath += "/";
			ipath += *i;

			if (x0::fileinfo_ptr fi = in.connection.server().fileinfo(ipath))
			{
				if (fi->is_regular())
				{
					in.fileinfo = fi;
					break;
				}
			}
		}
	}
};

X0_EXPORT_PLUGIN(indexfile);
