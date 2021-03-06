/* <plugins/director/DirectorPlugin.h>
 *
 * This file is part of the x0 web server project and is released under GPL-3.
 * http://www.xzero.io/
 *
 * (c) 2009-2012 Christian Parpart <trapni@gentoo.org>
 */

#pragma once

#include <x0/http/HttpPlugin.h>

#include <string>
#include <unordered_map>

namespace x0 {
	class HttpServer;
	class HttpRequest;
}

class Director;
class RoadWarrior;
class Backend;

class DirectorPlugin :
	public x0::HttpPlugin
{
private:
	std::unordered_map<std::string, Director*> directors_;
	RoadWarrior* roadWarrior_;

public:
	DirectorPlugin(x0::HttpServer& srv, const std::string& name);
	~DirectorPlugin();

private:
	void director_load(const x0::FlowParams& args, x0::FlowValue& result);
	void director_create(const x0::FlowParams& args, x0::FlowValue& result);

	void director_segment(x0::HttpRequest* r, const x0::FlowParams& args, x0::FlowValue& result);
	bool director_pass(x0::HttpRequest* r, const x0::FlowParams& args);
	bool director_api(x0::HttpRequest* r, const x0::FlowParams& args);
	bool director_fcgi(x0::HttpRequest* r, const x0::FlowParams& args);
	bool director_http(x0::HttpRequest* r, const x0::FlowParams& args);

	Director* createDirector(const char* id);
	Backend* registerBackend(Director* director, const char* name, const char* url);
};
