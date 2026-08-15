#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>

#include <control/commands/commands.h>
#include <control/fops/fops.h>
#include <control/session.h>
#include <transport/tcp.h>

static void mapple_drain_sessions(void)
{
	struct mapple_session *sessions[MAPPLE_MAX_SESSIONS];
	size_t count;
	size_t index;

	do {
		count = mapple_session_manager_snapshot(sessions, ARRAY_SIZE(sessions));
		for (index = 0; index < count; ++index)
			mapple_session_shutdown(sessions[index]);
		for (index = 0; index < count; ++index) {
			mapple_session_wait(sessions[index]);
			mapple_session_put(sessions[index]);
		}
	} while (count);
	WARN_ON(mapple_session_manager_active_count() != 0);
	WARN_ON(!mapple_session_manager_is_empty());
}

static int __init mapple_init(void)
{
	int ret;

	mapple_session_manager_init();
	ret = mapple_protocol_init();
	if (ret)
		return ret;
	ret = mapple_chardev_init();
	if (ret)
		return ret;
	ret = mapple_tcp_init();
	if (ret)
		goto stop_chardev;

	pr_info("mapple: module loaded (shared session limit %u)\n", MAPPLE_MAX_SESSIONS);
	return 0;

stop_chardev:
	mapple_session_manager_begin_shutdown();
	mapple_drain_sessions();
	mapple_chardev_exit();
	return ret;
}

static void __exit mapple_exit(void)
{
	mapple_session_manager_begin_shutdown();
	mapple_tcp_exit();
	mapple_drain_sessions();
	mapple_chardev_exit();
	pr_info("mapple: module unloaded\n");
}

module_init(mapple_init);
module_exit(mapple_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Shared binary protocol over chardev and TCP transports");
