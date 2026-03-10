# $FreeBSD$

PORTNAME=	spine
PORTVERSION=	1.3.0
CATEGORIES=	net-mgmt
MASTER_SITES=	https://github.com/Cacti/spine/archive/refs/tags/
DISTNAME=	${PORTVERSION}
DIST_SUBDIR=	${PORTNAME}

MAINTAINER=	developers@cacti.net
COMMENT=	High-performance SNMP poller for Cacti
WWW=		https://www.cacti.net/

LICENSE=	LGPL21
LICENSE_FILE=	${WRKSRC}/LICENSE

LIB_DEPENDS=	libmariadb.so:databases/mariadb106-client \
		libnetsnmp.so:net-mgmt/net-snmp \
		libcrypto.so:security/openssl

GNU_CONFIGURE=	yes
CONFIGURE_ARGS=	--bindir=${PREFIX}/sbin \
		--sysconfdir=${PREFIX}/etc
USE_AUTOTOOLS=	autoconf automake libtool
USE_RC_SUBR=	spine
BUILD_DEPENDS=	help2man:misc/help2man \
		dos2unix:converters/dos2unix

USERS=		cacti
GROUPS=		cacti

PLIST_FILES=	sbin/spine \
		etc/spine.conf.dist \
		man/man1/spine.1.gz \
		etc/rc.d/spine

SUB_FILES=	pkg-message

post-install:
	${INSTALL} -m 0640 ${WRKSRC}/spine.conf.dist \
		${STAGEDIR}${PREFIX}/etc/spine.conf.dist
	# spine needs CAP_NET_RAW for ICMP raw sockets. --enable-lcap is
	# Linux-specific (libcap POSIX.1e); FreeBSD does not use it.
	# Grant access via setuid-root, mac_portacl(4), or run Cacti's poller
	# as root. See pkg-message for site-policy options.

.include <bsd.port.mk>
