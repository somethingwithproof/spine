-- Minimal Cacti schema seed for spine integration tests.
-- Provides enough structure for spine to poll one SNMP host.

CREATE TABLE IF NOT EXISTS `host` (
  `id`                  int(10) unsigned NOT NULL AUTO_INCREMENT,
  `hostname`            varchar(250) NOT NULL DEFAULT '',
  `snmp_community`      varchar(100) NOT NULL DEFAULT '',
  `snmp_version`        tinyint(1) unsigned NOT NULL DEFAULT 1,
  `snmp_port`           mediumint(5) unsigned NOT NULL DEFAULT 161,
  `snmp_username`       varchar(50) NOT NULL DEFAULT '',
  `snmp_auth_protocol`  char(6) NOT NULL DEFAULT '',
  `snmp_auth_password`  varchar(200) NOT NULL DEFAULT '',
  `snmp_priv_protocol`  char(6) NOT NULL DEFAULT '',
  `snmp_priv_password`  varchar(200) NOT NULL DEFAULT '',
  `status`              tinyint(2) NOT NULL DEFAULT 0,
  `disabled`            char(2) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `poller_item` (
  `local_data_id`       mediumint(8) unsigned NOT NULL DEFAULT 0,
  `host_id`             mediumint(8) unsigned NOT NULL DEFAULT 0,
  `action`              tinyint(2) NOT NULL DEFAULT 1,
  `hostname`            varchar(250) NOT NULL DEFAULT '',
  `snmp_community`      varchar(100) NOT NULL DEFAULT '',
  `snmp_version`        tinyint(1) unsigned NOT NULL DEFAULT 1,
  `snmp_port`           mediumint(5) unsigned NOT NULL DEFAULT 161,
  `snmp_timeout`        int(10) unsigned NOT NULL DEFAULT 500,
  `snmp_username`       varchar(50) NOT NULL DEFAULT '',
  `snmp_auth_protocol`  char(6) NOT NULL DEFAULT '',
  `snmp_auth_password`  varchar(200) NOT NULL DEFAULT '',
  `snmp_priv_protocol`  char(6) NOT NULL DEFAULT '',
  `snmp_priv_password`  varchar(200) NOT NULL DEFAULT '',
  `rrd_name`            varchar(19) NOT NULL DEFAULT '',
  `rrd_path`            varchar(255) NOT NULL DEFAULT '',
  `arg1`                text,
  `arg2`                varchar(255) NOT NULL DEFAULT '',
  `arg3`                varchar(255) NOT NULL DEFAULT '',
  PRIMARY KEY (`local_data_id`, `rrd_name`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `poller_output` (
  `local_data_id` mediumint(8) unsigned NOT NULL DEFAULT 0,
  `rrd_name`      varchar(19) NOT NULL DEFAULT '',
  `time`          datetime NOT NULL DEFAULT '0000-00-00 00:00:00',
  `output`        text NOT NULL,
  PRIMARY KEY (`local_data_id`, `rrd_name`, `time`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Test host 1: SNMPv1, points to snmpsim serving 192.0.2.1 (RFC 5737 documentation range).
-- status=3 = up, community matches the .snmprec filename prefix.
INSERT INTO `host`
  (`id`, `hostname`, `snmp_community`, `snmp_version`, `snmp_port`,
   `snmp_username`, `snmp_auth_protocol`, `snmp_auth_password`,
   `snmp_priv_protocol`, `snmp_priv_password`, `status`, `disabled`)
VALUES
  (1, '192.0.2.1', 'public', 1, 161, '', '', '', '', '', 3, '');

-- Test host 2: SNMPv3 authPriv, points to snmpsim serving 192.0.2.2.
INSERT INTO `host`
  (`id`, `hostname`, `snmp_community`, `snmp_version`, `snmp_port`,
   `snmp_username`, `snmp_auth_protocol`, `snmp_auth_password`,
   `snmp_priv_protocol`, `snmp_priv_password`, `status`, `disabled`)
VALUES
  (2, '192.0.2.2', '', 3, 161, 'testuser', 'MD5', 'testauth123', 'DES', 'testpriv123', 3, '');

-- poller_item for host 1: sysDescr (OID 1.3.6.1.2.1.1.1.0).
-- action=1 = SNMP, arg1 carries the OID.
INSERT INTO `poller_item`
  (`local_data_id`, `host_id`, `action`, `hostname`, `snmp_community`,
   `snmp_version`, `snmp_port`, `snmp_timeout`,
   `snmp_username`, `snmp_auth_protocol`, `snmp_auth_password`,
   `snmp_priv_protocol`, `snmp_priv_password`,
   `rrd_name`, `rrd_path`, `arg1`, `arg2`, `arg3`)
VALUES
  (1, 1, 1, '192.0.2.1', 'public', 1, 161, 500, '', '', '', '', '',
   'sysDescr', '/var/lib/cacti/rrd/1/ds_1.rrd', '1.3.6.1.2.1.1.1.0', '', '');

-- poller_item for host 2: sysDescr via SNMPv3.
INSERT INTO `poller_item`
  (`local_data_id`, `host_id`, `action`, `hostname`, `snmp_community`,
   `snmp_version`, `snmp_port`, `snmp_timeout`,
   `snmp_username`, `snmp_auth_protocol`, `snmp_auth_password`,
   `snmp_priv_protocol`, `snmp_priv_password`,
   `rrd_name`, `rrd_path`, `arg1`, `arg2`, `arg3`)
VALUES
  (2, 2, 1, '192.0.2.2', '', 3, 161, 500, 'testuser', 'MD5', 'testauth123', 'DES', 'testpriv123',
   'sysDescr', '/var/lib/cacti/rrd/2/ds_1.rrd', '1.3.6.1.2.1.1.1.0', '', '');
