#ifndef sw_x0_SslSocket_h
#define sw_x0_SslSocket_h (1)

#include <x0/Socket.h>
#include <x0/sysconfig.h>

#if defined(WITH_SSL)
#	include <gnutls/gnutls.h>
#endif

namespace x0 {

class SslDriver;
class SslContext;

/** \brief SSL socket.
 */
class SslSocket :
	public Socket
{
private:
	ev_tstamp ctime_;

	SslDriver *driver_;
	const SslContext *context_;
	gnutls_session_t session_;		//!< SSL (GnuTLS) session handle

	friend class SslDriver;
	friend class SslContext;

	static int onClientHello(gnutls_session_t session);

public:
	explicit SslSocket(SslDriver *driver, int fd);
	virtual ~SslSocket();

	const SslContext *context() const;

public:
	// synchronous non-blocking I/O
	virtual ssize_t read(Buffer& result);
	virtual ssize_t write(const BufferRef& source);
	virtual ssize_t write(int fd, off_t *offset, size_t nbytes);

protected:
	virtual void handshake();
};

// {{{ inlines
inline const SslContext *SslSocket::context() const
{
	return context_;
}
// }}}

} // namespace x0

#endif
