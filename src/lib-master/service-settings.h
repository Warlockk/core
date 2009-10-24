#ifndef SERVICE_SETTINGS_H
#define SERVICE_SETTINGS_H

/* <settings checks> */
enum service_type {
	SERVICE_TYPE_UNKNOWN,
	SERVICE_TYPE_LOG,
	SERVICE_TYPE_ANVIL,
	SERVICE_TYPE_CONFIG,
	SERVICE_TYPE_LOGIN
};
/* </settings checks> */

struct file_listener_settings {
	const char *path;
	unsigned int mode;
	const char *user;
	const char *group;
};
ARRAY_DEFINE_TYPE(file_listener_settings, struct file_listener_settings *);

struct inet_listener_settings {
	const char *address;
	unsigned int port;
	bool ssl;
};

struct service_settings {
	const char *name;
	const char *protocol;
	const char *type;
	const char *executable;
	const char *user;
	const char *group;
	const char *privileged_group;
	const char *extra_groups;
	const char *chroot;

	bool drop_priv_before_exec;

	unsigned int process_min_avail;
	unsigned int process_limit;
	unsigned int client_limit;
	unsigned int service_count;
	unsigned int vsz_limit;

	ARRAY_TYPE(file_listener_settings) unix_listeners;
	ARRAY_TYPE(file_listener_settings) fifo_listeners;
	ARRAY_DEFINE(inet_listeners, struct inet_listener_settings *);

	/* internal to master: */
	struct master_settings *master_set;
	enum service_type parsed_type;
	unsigned int login_dump_core:1;
};

#endif
