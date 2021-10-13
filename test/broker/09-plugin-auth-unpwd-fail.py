#!/usr/bin/env python3

# Test whether a connection is successful with correct username and password
# when using a simple auth_plugin.

from dimq_test_helper import *

def write_config(filename, port, plugin_ver):
    with open(filename, 'w') as f:
        f.write("port %d\n" % (port))
        f.write("auth_plugin c/auth_plugin_v%d.so\n" % (plugin_ver))
        f.write("allow_anonymous false\n")

def do_test(plugin_ver):
    port = dimq_test.get_port()
    conf_file = os.path.basename(__file__).replace('.py', '.conf')
    write_config(conf_file, port, plugin_ver)

    rc = 1
    keepalive = 10
    connect_packet = dimq_test.gen_connect("connect-uname-pwd-test", keepalive=keepalive, username="test-username", password="wrong")
    connack_packet = dimq_test.gen_connack(rc=5)

    broker = dimq_test.start_broker(filename=os.path.basename(__file__), use_conf=True, port=port)

    try:
        sock = dimq_test.do_client_connect(connect_packet, connack_packet, timeout=20, port=port)
        rc = 0

        sock.close()
    except dimq_test.TestError:
        pass
    finally:
        os.remove(conf_file)
        broker.terminate()
        broker.wait()
        (stdo, stde) = broker.communicate()
        if rc:
            print(stde.decode('utf-8'))
            exit(rc)

do_test(4)
do_test(5)