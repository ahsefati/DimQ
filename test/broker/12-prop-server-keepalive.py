#!/usr/bin/env python3

# Test whether sending a non zero session expiry interval in DISCONNECT after
# having sent a zero session expiry interval is treated correctly in MQTT v5.

from dimq_test_helper import *

def write_config(filename, port):
    with open(filename, 'w') as f:
        f.write("port %d\n" % (port))
        f.write("allow_anonymous true\n")
        f.write("\n")
        f.write("max_keepalive 60\n")

port = dimq_test.get_port(1)
conf_file = os.path.basename(__file__).replace('.py', '.conf')
write_config(conf_file, port)


rc = 1

keepalive = 61
connect_packet = dimq_test.gen_connect("test", proto_ver=5, keepalive=keepalive)

props = mqtt5_props.gen_uint16_prop(mqtt5_props.PROP_SERVER_KEEP_ALIVE, 60) \
        + mqtt5_props.gen_uint16_prop(mqtt5_props.PROP_TOPIC_ALIAS_MAXIMUM, 10) \
        + mqtt5_props.gen_uint16_prop(mqtt5_props.PROP_RECEIVE_MAXIMUM, 20)
connack_packet = dimq_test.gen_connack(rc=0, proto_ver=5, properties=props, property_helper=False)

broker = dimq_test.start_broker(filename=os.path.basename(__file__), port=port, use_conf=True)

try:
    sock = dimq_test.do_client_connect(connect_packet, connack_packet, port=port)
    sock.close()
    rc = 0
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

