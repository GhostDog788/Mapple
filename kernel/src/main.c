#include <linux/init.h>
#include <linux/module.h>
#include <linux/printk.h>

#include <control/sessions/session_manager.h>
#include <control/transports/chardev/init.h>
#include <control/transports/tcp_server/tcp.h>

static int __init mapple_init(void)
{
	int ret;

	mapple_session_manager_init();
	ret = mapple_chardev_init();
	if (ret)
		return ret;
	ret = mapple_tcp_init();
	if (ret)
		goto stop_chardev;

	pr_info("mapple: module loaded (shared session limit %u)\n", MAPPLE_MAX_SESSIONS);
	return 0;

stop_chardev:
	mapple_session_manager_shutdown();
	mapple_chardev_exit();
	return ret;
}

static void __exit mapple_exit(void)
{
	mapple_session_manager_shutdown();
	mapple_tcp_exit();
	mapple_chardev_exit();
	pr_info("mapple: module unloaded\n");
}

module_init(mapple_init);
module_exit(mapple_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Kernel pentest tool");
