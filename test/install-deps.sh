#!/bin/sh
# install-deps.sh - Install spine build dependencies per OS.
# Auto-detects the OS and installs the right packages.
set -eu

log() { printf '[deps] %s\n' "$*"; }

if [ -f /etc/os-release ]; then
	. /etc/os-release
	DISTRO="$ID"
else
	DISTRO="$(uname -s | tr '[:upper:]' '[:lower:]')"
fi

log "Detected OS: $DISTRO (${VERSION_ID:-unknown})"

case "$DISTRO" in
	rocky|rhel|centos|almalinux)
		# Enable CRB/PowerTools for help2man on Rocky/RHEL
		dnf install -y --quiet epel-release 2>/dev/null || true
		dnf config-manager --set-enabled crb 2>/dev/null || \
			dnf config-manager --set-enabled powertools 2>/dev/null || true
		dnf install -y --quiet \
			gcc autoconf automake libtool make \
			mariadb-devel net-snmp-devel openssl-devel \
			dos2unix help2man
		;;
	opensuse*|sles)
		zypper --quiet --non-interactive install \
			gcc autoconf automake libtool make \
			libmysqlclient-devel net-snmp-devel libopenssl-devel \
			dos2unix help2man
		;;
	ubuntu|debian)
		export DEBIAN_FRONTEND=noninteractive
		apt-get update -qq
		apt-get install -y -qq \
			gcc autoconf automake libtool make \
			libmysqlclient-dev libsnmp-dev libssl-dev \
			dos2unix help2man
		;;
	freebsd)
		pkg install -y \
			autoconf automake libtool gmake help2man \
			mysql84-client net-snmp
		# FreeBSD uses gmake
		if ! command -v make >/dev/null 2>&1; then
			ln -sf /usr/local/bin/gmake /usr/local/bin/make
		fi
		;;
	*)
		log "Unknown OS: $DISTRO - install deps manually"
		exit 1
		;;
esac

log "Dependencies installed"
