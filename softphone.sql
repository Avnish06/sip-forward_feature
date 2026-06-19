-- Softphone test account: register Zoiper/MicroSIP/Linphone to Asterisk and
-- dial out through the synchrovox_out trunk. Bypasses the media-server entirely
-- so you can validate the Tata trunk on its own.
--
-- Load into the running DB:
--   docker compose -f global-asterisk-server-docker-compose.yml exec -T db \
--     mariadb -uasterisk -p'f3mb0yTw1nkyB@!!69' asterisk < softphone.sql
-- (no pjsip reload needed for realtime endpoint/auth/aor rows)

INSERT INTO ps_aors (id, max_contacts, qualify_frequency, remove_existing)
VALUES ('softphone', 1, 30, 'yes')
ON DUPLICATE KEY UPDATE max_contacts=VALUES(max_contacts);

INSERT INTO ps_auths (id, auth_type, username, password, realm)
VALUES ('softphone_auth', 'userpass', 'softphone', 'softphone123', 'asterisk')
ON DUPLICATE KEY UPDATE password=VALUES(password);

INSERT INTO ps_endpoints
  (id, transport, aors, auth, context, disallow, allow,
   direct_media, force_rport, rewrite_contact, rtp_symmetric, ice_support, from_user)
VALUES
  ('softphone', 'transport-udp', 'softphone', 'softphone_auth', 'softphone-out',
   'all', 'ulaw,alaw',
   'no', 'yes', 'yes', 'yes', 'no', 'softphone')
ON DUPLICATE KEY UPDATE context=VALUES(context);
