-- Create tables for PJSIP realtime configuration only if they don't exist
CREATE TABLE IF NOT EXISTS ps_endpoints (
    id VARCHAR(100) NOT NULL PRIMARY KEY,
    transport VARCHAR(40),
    aors VARCHAR(200),
    auth VARCHAR(40),
    context VARCHAR(40),
    disallow VARCHAR(200),
    allow VARCHAR(200),
    timers ENUM('yes','no'),
    direct_media ENUM('yes','no'),
    direct_media_method ENUM('invite','reinvite','update'),
    connected_line_method ENUM('invite','reinvite','update'),
    force_rport ENUM('yes','no'),
    rewrite_contact ENUM('yes','no'),
    rtp_symmetric ENUM('yes','no'),
    ice_support ENUM('yes','no'),
    from_user VARCHAR(40),
    from_domain VARCHAR(40),
    media_address VARCHAR(40),
    bind_rtp_to_media_address ENUM('yes','no'),
    media_use_received_transport ENUM('yes','no'),
    outbound_auth VARCHAR(40),
    mailboxes VARCHAR(200),
    incoming_mwi_mailbox VARCHAR(200),
    mwi_subscribe_replaces_unsolicited ENUM('yes','no'),
    message_context VARCHAR(40),
    mwi_from_user VARCHAR(40),
    mwi_max_messages INT DEFAULT 100,
    send_pai ENUM('yes','no') DEFAULT 'yes',
    send_rpid ENUM('yes','no') DEFAULT 'yes'
);

CREATE TABLE IF NOT EXISTS ps_aors (
    id VARCHAR(40) NOT NULL PRIMARY KEY,
    contact VARCHAR(255),
    max_contacts INT,
    qualify_frequency INT,
    remove_existing ENUM('yes','no')
);

CREATE TABLE IF NOT EXISTS ps_auths (
    id VARCHAR(40) NOT NULL PRIMARY KEY,
    auth_type ENUM('userpass','md5'),
    password VARCHAR(80),
    username VARCHAR(40),
    realm VARCHAR(40)
);

CREATE TABLE IF NOT EXISTS ps_contacts (
    id VARCHAR(255) NOT NULL PRIMARY KEY,
    uri VARCHAR(255),
    expiration_time VARCHAR(40),
    qualify_frequency INT,
    outbound_proxy VARCHAR(255),
    path TEXT,
    user_agent VARCHAR(255),
    qualify_timeout INT,
    reg_server VARCHAR(255),
    authenticate_qualify ENUM('yes','no')
);

-- Create table for storing webhook URLs for each trunk
CREATE TABLE IF NOT EXISTS trunk_webhooks (
    trunk_id VARCHAR(40) NOT NULL PRIMARY KEY,
    answer_webhook_url TEXT NOT NULL,
    FOREIGN KEY (trunk_id) REFERENCES ps_endpoints(id) ON DELETE CASCADE
);

-- Create table for outbound registrations
CREATE TABLE IF NOT EXISTS ps_registrations (
    id VARCHAR(40) NOT NULL PRIMARY KEY,
    auth_rejection_permanent ENUM('yes','no'),
    client_uri VARCHAR(255),
    contact_user VARCHAR(40),
    expiration INT,
    max_retries INT,
    outbound_auth VARCHAR(40),
    outbound_proxy VARCHAR(255),
    retry_interval INT,
    forbidden_retry_interval INT,
    server_uri VARCHAR(255),
    transport VARCHAR(40),
    support_path ENUM('yes','no'),
    fatal_retry_interval INT,
    line ENUM('yes','no'),
    endpoint VARCHAR(40)
);

CREATE TABLE IF NOT EXISTS ps_domain_aliases (
    id VARCHAR(40) NOT NULL PRIMARY KEY,
    endpoint VARCHAR(40),
    `match` VARCHAR(80)
);

CREATE TABLE IF NOT EXISTS ps_provider_dids (
    id                  VARCHAR(100) NOT NULL PRIMARY KEY,
    did                 VARCHAR(40)  NOT NULL,
    provider_id         VARCHAR(100) NOT NULL,
    answer_webhook_url  TEXT,
    
    CONSTRAINT fk_provider
    FOREIGN KEY (provider_id)
    REFERENCES ps_endpoints(id)
);
