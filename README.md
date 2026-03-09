# Spine: a poller for Cacti

Spine is a high speed poller replacement for `cmd.php`. It is almost 100%
compatible with the legacy cmd.php processor and provides much more flexibility,
speed and concurrency than `cmd.php`.

Make sure that you have the proper development environment to compile Spine.
This includes compilers, header files and things such as libtool. If you have
questions please consult the forums and/or online documentation.

-----------------------------------------------------------------------------

## Build Dependencies

Spine requires GCC (or Clang), autoconf/automake/libtool, a MariaDB/MySQL
client library, Net-SNMP, and OpenSSL. Install them for your platform before
running the Unix build steps below.

### Debian / Ubuntu

```shell
sudo apt-get update
sudo apt-get install -y \
    build-essential autoconf automake libtool help2man \
    libmariadb-dev libsnmp-dev libssl-dev libcap-dev
```

### RHEL / Rocky Linux / AlmaLinux / CentOS Stream

```shell
sudo dnf install -y \
    gcc autoconf automake libtool help2man \
    mariadb-devel net-snmp-devel openssl-devel libcap-devel
```

On RHEL 8/9 you may need to enable the CRB/CodeReady repository first:

```shell
sudo dnf config-manager --set-enabled crb   # RHEL 9 / Rocky 9
sudo dnf config-manager --set-enabled powertools  # CentOS Stream 8
```

### openSUSE / SLES

```shell
sudo zypper install -y \
    gcc autoconf automake libtool help2man \
    libmariadb-devel net-snmp-devel libopenssl-devel libcap-devel
```

### Alpine Linux

```shell
apk add --no-cache \
    gcc musl-dev make autoconf automake libtool help2man \
    mariadb-dev net-snmp-dev openssl-dev libcap-dev
```

### Arch Linux / Manjaro

```shell
sudo pacman -S --needed \
    base-devel autoconf automake libtool help2man \
    mariadb-libs net-snmp openssl libcap
```

### macOS (Homebrew)

```shell
brew install autoconf automake libtool help2man \
    mariadb-connector-c net-snmp openssl@3
```

Then configure with OpenSSL from Homebrew:

```shell
./configure LDFLAGS="-L$(brew --prefix openssl@3)/lib" \
            CPPFLAGS="-I$(brew --prefix openssl@3)/include"
```

`libcap` is Linux-only; `--enable-lcap` is not supported on macOS.

### FreeBSD

```shell
pkg install -y autoconf automake libtool help2man \
    mariadb106-client net-snmp openssl
```

FreeBSD uses `gmake` instead of `make`:

```shell
./bootstrap
./configure
gmake
sudo gmake install
```

### NetBSD / OpenBSD

Install the equivalent packages via `pkgsrc` or `pkg_add`, then use `gmake`.
The `libcap` dependency is Linux-specific; omit `--enable-lcap`.

-----------------------------------------------------------------------------

## Unix Installation

These instructions assume the default install location for spine of
`/usr/local/spine`. If you choose to use another prefix, make sure you update
the commands as required for that new path.

To compile and install Spine using MySQL versions 5.5 or higher please do the
following:

```shell
./bootstrap
./configure
make
make install
chown root:root /usr/local/spine/bin/spine
chmod u+s /usr/local/spine/bin/spine
```

To compile and install Spine using MySQL versions previous to 5.5 please do the
following:

```shell
./bootstrap
./configure --with-reentrant
make
make install
chown root:root /usr/local/spine/bin/spine
chmod +s /usr/local/spine/bin/spine
```

## Windows Installation

### CYGWIN Prerequisite

1. Download Cygwin for Window from [https://www.cygwin.com/](https://www.cygwin.com/)

2. Install Cygwin by executing the downloaded setup program

3. Select _Install from Internet_

4. Select Root Directory:  _C:\cygwin_

5. Select a mirror which is close to your location

6. Once on the package selection section make sure to select the following (TIP:
   use the search!):

   * autoconf
   * automake
   * dos2unix
   * gcc-core
   * gzip
   * help2man
   * inetutils-src
   * libmysqlclient
   * libmariadb-devel
   * libssl-devel
   * libtool
   * m4
   * make
   * net-snmp-devel
   * openssl-devel
   * wget

7. Wait for installation to complete, coffee time!

8. Move the cygwin setup to the C:\cygwin\ folder for future usage.

### Compile Spine

1. Open Cygwin shell prompt (C:\Cygwin\cygwin.bat) and brace yourself to use
   unix commands on Windows.

2. Download the Spine source to the current directory:

   [http://www.cacti.net/spine_download.php](http://www.cacti.net/spine_download.php)

3. Extract Spine into C:\Cygwin\usr\src\<spineversion>:

   `tar xzvf cacti-spine-*.tar.gz`

4. Change into the Spine directory:

   `cd /usr/src/cacti-spine-*`

5. Run bootstrap to prepare Spine for compilation:

   `./bootstrap`

6. Follow the instruction which bootstrap outputs.

7. Update the spine.conf file for your installation of Cacti. You can optionally
   move it to a better location if you choose to do so, make sure to copy the
   spine.conf as well.

8. Ensure that Spine runs well by running with `/usr/local/spine/spine -R -S -V 3`

9. Update Cacti `Paths` Setting to point to the Spine binary and update the
   `Poller Type` to Spine. For the spine binary on Windows x64, and using default
   locations, that would be `C:\cygwin64\usr\local\spine\bin\spine.exe`

10. If all is good Spine will be run from the poller in place of cmd.php.

## Known Issues

1. On Windows, Microsoft does not support a TCP Socket send timeout. Therefore,
   if you are using TCP ping on Windows, spine will not perform a second or
   subsequent retries to connect and the host will be assumed down on the first
   failure.

   If this is a problem it is suggested to use another Availability/Reachability
   method, or moving to Linux/UNIX.

2. Spine takes quite a few MySQL connections. The number of connections is
   calculated as follows: (1 for main poller + 1 per each thread + 1 per each
   script server)

   Therefore, if you have 4 processes, with 10 threads each, and 5 script
   servers each your spine will take approximately:

   `total connections = 4 * ( 1 + 10 + 5 ) = 64`

3. On older MySQL versions, different libraries had to be used to make MySQL
   thread safe. MySQL versions 5.0 and 5.1 require this flag. If you are using
   these version of MySQL, you must use the --with-reentrant configure flag.

-----------------------------------------------------------------------------
Copyright (c) 2004-2026 - The Cacti Group, Inc.
