#!/bin/bash
# Two-container mode: run MariaDB *inside* the Asterisk container so the whole
# stack is just two containers (asterisk+db, media-server). Initializes + seeds
# the DB on first boot, then launches Asterisk in the foreground.
set -e

DATADIR=/var/lib/mysql
SOCKDIR=/run/mysqld
SOCK="$SOCKDIR/mysqld.sock"
DB_PASS='f3mb0yTw1nkyB@!!69'

# Socket dir is required or mariadbd dies with "Bind on unix socket: No such file"
install -d -o mysql -g mysql "$SOCKDIR"
install -d -o mysql -g mysql "$DATADIR"
chown -R mysql:mysql "$DATADIR" "$SOCKDIR"

# Initialize system tables only if truly empty
if [ ! -d "$DATADIR/mysql" ]; then
  echo "[entrypoint] Fresh data dir -> initializing MariaDB system tables..."
  mariadb-install-db --user=mysql --datadir="$DATADIR" --auth-root-authentication-method=normal >/dev/null 2>&1
fi

echo "[entrypoint] Starting MariaDB in background..."
/usr/sbin/mariadbd --user=mysql --datadir="$DATADIR" --socket="$SOCK" --bind-address=127.0.0.1 &

echo "[entrypoint] Waiting for MariaDB to accept connections..."
for i in $(seq 1 60); do
  if mariadb --socket="$SOCK" -uroot -e "SELECT 1" >/dev/null 2>&1; then
    echo "[entrypoint] MariaDB is up."
    break
  fi
  sleep 1
done

# Seed schema/user only if the asterisk schema is missing (idempotent — works
# whether the data dir was empty or pre-initialized by the mariadb-server package)
if ! mariadb --socket="$SOCK" -uroot -e "USE asterisk; SELECT 1 FROM ps_endpoints LIMIT 1" >/dev/null 2>&1; then
  echo "[entrypoint] Seeding asterisk database, user and schema..."
  mariadb --socket="$SOCK" -uroot <<SQL
CREATE DATABASE IF NOT EXISTS asterisk CHARACTER SET utf8;
CREATE USER IF NOT EXISTS 'asterisk'@'localhost' IDENTIFIED BY '${DB_PASS}';
CREATE USER IF NOT EXISTS 'asterisk'@'127.0.0.1' IDENTIFIED BY '${DB_PASS}';
CREATE USER IF NOT EXISTS 'asterisk'@'%'         IDENTIFIED BY '${DB_PASS}';
GRANT ALL PRIVILEGES ON asterisk.* TO 'asterisk'@'localhost';
GRANT ALL PRIVILEGES ON asterisk.* TO 'asterisk'@'127.0.0.1';
GRANT ALL PRIVILEGES ON asterisk.* TO 'asterisk'@'%';
FLUSH PRIVILEGES;
SQL
  mariadb --socket="$SOCK" -uroot asterisk < /etc/asterisk/asterisk.sql
  mariadb --socket="$SOCK" -uroot asterisk < /etc/asterisk/pjsip_config.sql
  echo "[entrypoint] DB seed complete."
else
  echo "[entrypoint] asterisk schema already present -> skipping seed."
fi

echo "[entrypoint] Starting Asterisk (foreground)..."
exec /usr/sbin/asterisk -f
