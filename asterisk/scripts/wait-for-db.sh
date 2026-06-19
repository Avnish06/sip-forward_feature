#!/bin/bash

host="$1"
user="$2"
password="$3"
database="$4"
shift 4  # Remove the first 4 arguments

until mariadb -h "$host" -u "$user" -p"$password" "$database" -e 'SELECT 1' >/dev/null 2>&1; do
  echo 'Waiting for database connection...'
  sleep 3
done

echo 'Database connection successful!'
exec "$@"  # Execute the remaining arguments as a command