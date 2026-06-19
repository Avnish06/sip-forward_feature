-- ==================================================================================================================================================================================================================
-- Media Servers Registration for Calls Routing
-- ==================================================================================================================================================================================================================

-- Insert India media server endpoint configuration
INSERT INTO ps_endpoints (id, transport, aors, auth, context, disallow, allow, direct_media, force_rport, rewrite_contact, rtp_symmetric, ice_support, from_user, from_domain, direct_media_method, connected_line_method, media_use_received_transport, timers)
VALUES ('india_media_dev', 'transport-udp', 'india_media_dev', 'india_media_dev_auth', 'internal', 'all', 'ulaw,alaw', 'yes', 'yes', 'yes', 'yes', 'no', 'india_media_dev', '34.23.9.86', 'reinvite', 'reinvite', 'yes', 'no');

-- Insert India media server auth
INSERT INTO ps_auths (id, auth_type, username, password, realm)
VALUES ('india_media_dev_auth', 'userpass', 'india_media_dev', 'secret123', 'asterisk');

-- Insert India media server AOR
INSERT INTO ps_aors (id, contact, max_contacts, qualify_frequency, remove_existing)
VALUES ('india_media_dev', NULL, 20, 60, 'no');

-- Insert India media server endpoint configuration
INSERT INTO ps_endpoints (id, transport, aors, auth, context, disallow, allow, direct_media, force_rport, rewrite_contact, rtp_symmetric, ice_support, from_user, from_domain, direct_media_method, connected_line_method, media_use_received_transport, timers)
VALUES ('india_media', 'transport-udp', 'india_media', 'india_media_auth', 'internal', 'all', 'ulaw,alaw', 'yes', 'yes', 'yes', 'yes', 'no', 'india_media', '34.21.21.165', 'reinvite', 'reinvite', 'yes', 'no');

-- Insert India media server auth
INSERT INTO ps_auths (id, auth_type, username, password, realm)
VALUES ('india_media_auth', 'userpass', 'india_media', 'secret123', 'asterisk');

-- Insert India media server AOR
INSERT INTO ps_aors (id, contact, max_contacts, qualify_frequency, remove_existing)
VALUES ('india_media', NULL, 20, 60, 'no');


-- Insert India 2 media server endpoint configuration
INSERT INTO ps_endpoints (id, transport, aors, auth, context, disallow, allow, direct_media, force_rport, rewrite_contact, rtp_symmetric, ice_support, from_user, from_domain, direct_media_method, connected_line_method, media_use_received_transport, timers)
VALUES ('india_media2', 'transport-udp', 'india_media2', 'india_media2_auth', 'internal', 'all', 'ulaw,alaw', 'yes', 'yes', 'yes', 'yes', 'no', 'india_media2', '35.230.170.115', 'reinvite', 'reinvite', 'yes', 'no');

-- Insert India media server auth
INSERT INTO ps_auths (id, auth_type, username, password, realm)
VALUES ('india_media2_auth', 'userpass', 'india_media2', 'secret123', 'asterisk');

-- Insert India media server AOR
INSERT INTO ps_aors (id, contact, max_contacts, qualify_frequency, remove_existing)
VALUES ('india_media2', NULL, 20, 60, 'no');


-- Insert India 3 media server endpoint configuration
INSERT INTO ps_endpoints (id, transport, aors, auth, context, disallow, allow, direct_media, force_rport, rewrite_contact, rtp_symmetric, ice_support, from_user, from_domain, direct_media_method, connected_line_method, media_use_received_transport, timers)
VALUES ('india_media3', 'transport-udp', 'india_media3', 'india_media3_auth', 'internal', 'all', 'ulaw,alaw', 'yes', 'yes', 'yes', 'yes', 'no', 'india_media3', '35.221.22.89', 'reinvite', 'reinvite', 'yes', 'no');

-- Insert India media server auth
INSERT INTO ps_auths (id, auth_type, username, password, realm)
VALUES ('india_media3_auth', 'userpass', 'india_media3', 'secret123', 'asterisk');

-- Insert India media server AOR
INSERT INTO ps_aors (id, contact, max_contacts, qualify_frequency, remove_existing)
VALUES ('india_media3', NULL, 20, 60, 'no');

-- Insert India 4 media server endpoint configuration
INSERT INTO ps_endpoints (id, transport, aors, auth, context, disallow, allow, direct_media, force_rport, rewrite_contact, rtp_symmetric, ice_support, from_user, from_domain, direct_media_method, connected_line_method, media_use_received_transport, timers)
VALUES ('india_media4', 'transport-udp', 'india_media4', 'india_media4_auth', 'internal', 'all', 'ulaw,alaw', 'yes', 'yes', 'yes', 'yes', 'no', 'india_media4', '35.186.162.79', 'reinvite', 'reinvite', 'yes', 'no');

-- Insert India media server auth
INSERT INTO ps_auths (id, auth_type, username, password, realm)
VALUES ('india_media4_auth', 'userpass', 'india_media4', 'secret123', 'asterisk');

-- Insert India media server AOR
INSERT INTO ps_aors (id, contact, max_contacts, qualify_frequency, remove_existing)
VALUES ('india_media4', NULL, 20, 60, 'no');

-- Insert India 5 media server endpoint configuration
INSERT INTO ps_endpoints (id, transport, aors, auth, context, disallow, allow, direct_media, force_rport, rewrite_contact, rtp_symmetric, ice_support, from_user, from_domain, direct_media_method, connected_line_method, media_use_received_transport, timers)
VALUES ('india_media5', 'transport-udp', 'india_media5', 'india_media5_auth', 'internal', 'all', 'ulaw,alaw', 'yes', 'yes', 'yes', 'yes', 'no', 'india_media5', '34.86.82.6', 'reinvite', 'reinvite', 'yes', 'no');

-- Insert India media server auth
INSERT INTO ps_auths (id, auth_type, username, password, realm)
VALUES ('india_media5_auth', 'userpass', 'india_media5', 'secret123', 'asterisk');

-- Insert India media server AOR
INSERT INTO ps_aors (id, contact, max_contacts, qualify_frequency, remove_existing)
VALUES ('india_media5', NULL, 20, 60, 'no');

-- Insert India 6 media server endpoint configuration
INSERT INTO ps_endpoints (id, transport, aors, auth, context, disallow, allow, direct_media, force_rport, rewrite_contact, rtp_symmetric, ice_support, from_user, from_domain, direct_media_method, connected_line_method, media_use_received_transport, timers)
VALUES ('india_media6', 'transport-udp', 'india_media6', 'india_media6_auth', 'internal', 'all', 'ulaw,alaw', 'yes', 'yes', 'yes', 'yes', 'no', 'india_media6', '34.85.131.83', 'reinvite', 'reinvite', 'yes', 'no');

-- Insert India media server auth
INSERT INTO ps_auths (id, auth_type, username, password, realm)
VALUES ('india_media6_auth', 'userpass', 'india_media6', 'secret123', 'asterisk');

-- Insert India media server AOR
INSERT INTO ps_aors (id, contact, max_contacts, qualify_frequency, remove_existing)
VALUES ('india_media6', NULL, 20, 60, 'no');

-- Insert India Aws 1 media server endpoint configuration
INSERT INTO ps_endpoints (id, transport, aors, auth, context, disallow, allow, direct_media, force_rport, rewrite_contact, rtp_symmetric, ice_support, from_user, from_domain, direct_media_method, connected_line_method, media_use_received_transport, timers)
VALUES ('india_media_aws_1', 'transport-udp', 'india_media_aws_1', 'india_media_aws_1_auth', 'internal', 'all', 'ulaw,alaw', 'yes', 'yes', 'yes', 'yes', 'no', 'india_media_aws_1', '34.200.228.210', 'reinvite', 'reinvite', 'yes', 'no');

-- Insert India media server auth
INSERT INTO ps_auths (id, auth_type, username, password, realm)
VALUES ('india_media_aws_1_auth', 'userpass', 'india_media_aws_1', 'secret123', 'asterisk');

-- Insert India media server AOR
INSERT INTO ps_aors (id, contact, max_contacts, qualify_frequency, remove_existing)
VALUES ('india_media_aws_1', NULL, 20, 60, 'no');

-- Insert India Aws 2 media server endpoint configuration
INSERT INTO ps_endpoints (id, transport, aors, auth, context, disallow, allow, direct_media, force_rport, rewrite_contact, rtp_symmetric, ice_support, from_user, from_domain, direct_media_method, connected_line_method, media_use_received_transport, timers)
VALUES ('india_media_aws_2', 'transport-udp', 'india_media_aws_2', 'india_media_aws_2_auth', 'internal', 'all', 'ulaw,alaw', 'yes', 'yes', 'yes', 'yes', 'no', 'india_media_aws_2', '32.194.169.109', 'reinvite', 'reinvite', 'yes', 'no');

-- Insert India media server auth
INSERT INTO ps_auths (id, auth_type, username, password, realm)
VALUES ('india_media_aws_2_auth', 'userpass', 'india_media_aws_2', 'secret123', 'asterisk');

-- Insert India media server AOR
INSERT INTO ps_aors (id, contact, max_contacts, qualify_frequency, remove_existing)
VALUES ('india_media_aws_2', NULL, 20, 60, 'no');

-- Insert India Aws 3 media server endpoint configuration
INSERT INTO ps_endpoints (id, transport, aors, auth, context, disallow, allow, direct_media, force_rport, rewrite_contact, rtp_symmetric, ice_support, from_user, from_domain, direct_media_method, connected_line_method, media_use_received_transport, timers)
VALUES ('india_media_aws_3', 'transport-udp', 'india_media_aws_3', 'india_media_aws_3_auth', 'internal', 'all', 'ulaw,alaw', 'yes', 'yes', 'yes', 'yes', 'no', 'india_media_aws_3', '3.227.235.243', 'reinvite', 'reinvite', 'yes', 'no');

-- Insert India media server auth
INSERT INTO ps_auths (id, auth_type, username, password, realm)
VALUES ('india_media_aws_3_auth', 'userpass', 'india_media_aws_3', 'secret123', 'asterisk');

-- Insert India media server AOR
INSERT INTO ps_aors (id, contact, max_contacts, qualify_frequency, remove_existing)
VALUES ('india_media_aws_3', NULL, 20, 60, 'no');

-- ==================================================================================================================================================================================================================
-- Providers Gateway Configurations for Inbound Calls (IP-based)
-- ==================================================================================================================================================================================================================

-- Insert Elision provider endpoint configuration
INSERT INTO ps_domain_aliases (id, endpoint, `match`) VALUES ('elision_questionalble_inbound_ip', 'elision_questionalble_inbound', '15.207.47.203');

INSERT INTO ps_domain_aliases (id, endpoint, `match`) VALUES ('elision_old_ip', 'elision_old', '103.20.104.146');

INSERT INTO ps_domain_aliases (id, endpoint, `match`) VALUES ('elision_greeter_ip', 'elision_greeter', '103.20.104.213');

INSERT INTO ps_domain_aliases (id, endpoint, `match`) VALUES ('elision_voicelink_ip', 'elision_voicelink', '160.30.71.89');

INSERT INTO ps_domain_aliases (id, endpoint, `match`) VALUES ('mcube_ip', 'mcube', '13.202.194.207');
