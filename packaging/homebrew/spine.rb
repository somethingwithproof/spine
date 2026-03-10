class Spine < Formula
  desc "Multithreaded poller for Cacti"
  homepage "https://www.cacti.net/"
  url "https://github.com/Cacti/spine/archive/refs/tags/spine-#{version}.tar.gz"
  # sha256 must be updated on each release; compute with:
  #   curl -sL <url> | shasum -a 256
  sha256 "0000000000000000000000000000000000000000000000000000000000000000"
  license "LGPL-2.1-or-later"

  head "https://github.com/Cacti/spine.git", branch: "develop"

  depends_on "autoconf" => :build
  depends_on "automake" => :build
  depends_on "libtool" => :build
  depends_on "mariadb-connector-c"
  depends_on "net-snmp"
  depends_on "openssl@3"

  def install
    system "autoreconf", "-fiv"
    system "./configure",
           "--disable-silent-rules",
           "--prefix=#{prefix}",
           "--with-mysql=#{Formula["mariadb-connector-c"].opt_prefix}",
           "--with-snmp=#{Formula["net-snmp"].opt_prefix}",
           "--with-openssl=#{Formula["openssl@3"].opt_prefix}"
    system "make", "install"

    # Install default config; generate a local copy the user can edit.
    etc.install "spine.conf.dist"
    unless (etc/"spine.conf").exist?
      cp etc/"spine.conf.dist", etc/"spine.conf"
    end
  end

  def post_install
    # Cacti's log directory; spine writes here when logging is enabled.
    (var/"log/cacti").mkpath
  end

  def caveats
    <<~EOS
      Edit #{etc}/spine.conf and set your Cacti database credentials:
        DB_Host      127.0.0.1
        DB_Database  cacti
        DB_User      cactiuser
        DB_Pass      <password>
        DB_Port      3306

      Raw-socket note: macOS does not support Linux file capabilities
      (setcap). ICMP ping checks require a raw socket, which on macOS
      demands root privileges or a setuid-root binary. Options:
        1. Run spine as root (not recommended for production).
        2. Configure Cacti to use fping (brew install fping), which is
           installed setuid-root by Homebrew and does not require spine
           to run as root.

      Homebrew installs spine to #{bin}/spine (not /usr/local/sbin).
      Update the spine_path setting in Cacti's configuration accordingly.
    EOS
  end

  test do
    system "#{bin}/spine", "--version"
  end
end
