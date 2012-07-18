/* <HttpServer.h>
 *
 * This file is part of the x0 web server project and is released under LGPL-3.
 * http://www.xzero.io/
 *
 * (c) 2009-2010 Christian Parpart <trapni@gentoo.org>
 */

#ifndef sw_x0_http_server_h
#define sw_x0_http_server_h (1)

#include <x0/io/FileInfoService.h>
#include <x0/http/HttpWorker.h>
#include <x0/http/Types.h>
#include <x0/ServerSocket.h>
#include <x0/DateTime.h>
#include <x0/TimeSpan.h>
#include <x0/Property.h>
#include <x0/Library.h>
#include <x0/Logger.h>
#include <x0/Signal.h>
#include <x0/Types.h>
#include <x0/Api.h>

#include <x0/sysconfig.h>

#include <x0/flow/FlowBackend.h>
#include <x0/flow/FlowRunner.h>

#include <cstring>
#include <string>
#include <memory>
#include <future>
#include <list>
#include <map>

#include <ev++.h>

class x0d; // friend declared in HttpServer

namespace x0 {

class SocketSpec;

//! \addtogroup http
//@{

class HttpPlugin;
class HttpCore;
class HttpWorker;

/**
 * \brief implements the x0 web server.
 *
 * \see HttpConnection, HttpRequest, HttpPlugin
 * \see HttpServer::run(), HttpServer::stop()
 */
class X0_API HttpServer :
#ifndef NDEBUG
	public Logging,
#endif
	public FlowBackend
{
	HttpServer(const HttpServer&) = delete;
	HttpServer& operator=(const HttpServer&) = delete;

public:
	typedef Signal<void(HttpConnection*)> ConnectionHook;
	typedef Signal<void(HttpRequest*)> RequestHook;
	typedef Signal<void(HttpWorker*)> WorkerHook;

public:
	explicit HttpServer(struct ::ev_loop* loop, unsigned generation = 1);
	~HttpServer();

	void setLogger(std::shared_ptr<Logger> logger);
	Logger* logger() const;

	ev_tstamp startupTime() const { return startupTime_; }
	ev_tstamp uptime() const { return ev_now(loop_) - startupTime_; }

	HttpWorker* spawnWorker();
	HttpWorker* selectWorker();
	const std::vector<HttpWorker*>& workers() const { return workers_; }
	void destroyWorker(HttpWorker* worker);

	// {{{ service control
	bool setup(std::istream* settings, const std::string& filename = std::string(), int optimizationLevel = 2);
	bool setup(const std::string& filename, int optimizationLevel = 2);
	int run();
	void stop();
	void kill();
	// }}}

	// {{{ signals raised on request in order
	ConnectionHook onConnectionOpen;	//!< This hook is invoked once a new client has connected.
	RequestHook onPreProcess; 			//!< is called at the very beginning of a request.
	RequestHook onResolveDocumentRoot;	//!< resolves document_root to use for this request.
	RequestHook onResolveEntity;		//!< maps the request URI into local physical path.
	RequestHook onPostProcess;			//!< gets invoked right before serializing headers
	RequestHook onRequestDone;			//!< this hook is invoked once the request has been <b>fully</b> served to the client.
	ConnectionHook onConnectionClose;	//!< is called before a connection gets closed / or has been closed by remote point.
	// }}}

	WorkerHook onWorkerSpawn;
	WorkerHook onWorkerUnspawn;

	unsigned generation() const { return generation_; }

	void addComponent(const std::string& value);

	/**
	 * writes a log entry into the server's error log.
	 */
	void log(Severity s, const char *msg, ...);

	template<typename... Args>
	inline void debug(int level, const char *msg, Args&&... args)
	{
#if !defined(NDEBUG)
		if (level <= logLevel_)
			log(Severity(level), msg, args...);
#endif
	}

	Severity logLevel() const;
	void logLevel(Severity value);

	ServerSocket* setupListener(const std::string& bindAddress, int port, int backlog = 0 /*default*/);
	ServerSocket* setupUnixListener(const std::string& path, int backlog = 0 /*default*/);
	ServerSocket* setupListener(const SocketSpec& spec);
	void destroyListener(ServerSocket* listener);

	std::string pluginDirectory() const;
	void setPluginDirectory(const std::string& value);

	HttpPlugin* loadPlugin(const std::string& name, std::error_code& ec);
	template<typename T> T* loadPlugin(const std::string& name, std::error_code& ec);
	void unloadPlugin(const std::string& name);
	std::vector<std::string> pluginsLoaded() const;

	HttpPlugin* registerPlugin(HttpPlugin* plugin);
	HttpPlugin* unregisterPlugin(HttpPlugin* plugin);

	struct ::ev_loop* loop() const;

	HttpCore& core() const;

	const std::list<ServerSocket*>& listeners() const;

	ServerSocket* listenerByHost(const std::string& hostid) const;
	ServerSocket* listenerByPort(int port) const;

	void dumpIR() const; // for debugging purpose

	HttpServer* fromText(const std::string& configText, std::future<int>** async = nullptr);

	friend class HttpConnection;
	friend class HttpPlugin;
	friend class HttpWorker;
	friend class HttpCore;

public: // FlowBackend overrides
	virtual void import(const std::string& name, const std::string& path);

	// setup
	bool registerSetupFunction(const std::string& name, const FlowValue::Type returnType, CallbackFunction callback, void* userdata = nullptr);
	bool registerSetupProperty(const std::string& name, const FlowValue::Type returnType, CallbackFunction callback, void* userdata = nullptr);

	// shared
	bool registerSharedFunction(const std::string& name, const FlowValue::Type returnType, CallbackFunction callback, void* userdata = nullptr);
	bool registerSharedProperty(const std::string& name, const FlowValue::Type returnType, CallbackFunction callback, void* userdata = nullptr);

	// main
	bool registerHandler(const std::string& name, CallbackFunction callback, void* userdata = nullptr);
	bool registerFunction(const std::string& name, const FlowValue::Type returnType, CallbackFunction callback, void* userdata = nullptr);
	bool registerProperty(const std::string& name, const FlowValue::Type returnType, CallbackFunction callback, void* userdata = nullptr);

private:
#if defined(WITH_SSL)
	static void gnutls_log(int level, const char* msg);
#endif

	static void* runWorker(void*);

	bool validateConfig();

	void onNewConnection(Socket*, ServerSocket*);

	unsigned generation_;
	std::vector<std::string> components_;

	Unit* unit_;
	FlowRunner* runner_;
	std::vector<std::string> setupApi_;
	std::vector<std::string> mainApi_;
	bool (*onHandleRequest_)(void*);

	std::list<ServerSocket*> listeners_;
	struct ::ev_loop* loop_;
	ev_tstamp startupTime_;
	LoggerPtr logger_;
	Severity logLevel_;
	bool colored_log_;
	std::string pluginDirectory_;
	std::vector<HttpPlugin*> plugins_;
	std::map<HttpPlugin*, Library> pluginLibraries_;
	HttpCore* core_;
	std::vector<HttpWorker*> workers_;
#if defined(X0_WORKER_RR)
	size_t lastWorker_;
#endif
	FileInfoService::Config fileinfoConfig_;

public:
	ValueProperty<std::size_t> maxConnections;
	ValueProperty<TimeSpan> maxKeepAlive;
	ValueProperty<std::size_t> maxKeepAliveRequests;
	ValueProperty<TimeSpan> maxReadIdle;
	ValueProperty<TimeSpan> maxWriteIdle;
	ValueProperty<bool> tcpCork;
	ValueProperty<bool> tcpNoDelay;
	ValueProperty<std::string> tag;
	ValueProperty<bool> advertise;

	ValueProperty<std::size_t> maxRequestUriSize;
	ValueProperty<std::size_t> maxRequestHeaderSize;
	ValueProperty<std::size_t> maxRequestHeaderCount;
	ValueProperty<std::size_t> maxRequestBodySize;
};

// {{{ inlines
inline void HttpServer::setLogger(std::shared_ptr<Logger> logger)
{
	logger_ = logger;
	logger_->setLevel(logLevel_);
}

inline Logger *HttpServer::logger() const
{
	return logger_.get();
}

inline struct ::ev_loop* HttpServer::loop() const
{
	return loop_;
}

inline HttpCore& HttpServer::core() const
{
	return *core_;
}

inline const std::list<ServerSocket*>& HttpServer::listeners() const
{
	return listeners_;
}

template<typename T>
inline T *HttpServer::loadPlugin(const std::string& name, std::error_code& ec)
{
	return dynamic_cast<T*>(loadPlugin(name, ec));
}

inline Severity HttpServer::logLevel() const
{
	return logLevel_;
}

inline void HttpServer::logLevel(Severity value)
{
	logLevel_ = value;
	logger()->setLevel(value);
}
// }}}

//@}

} // namespace x0

#endif
// vim:syntax=cpp
