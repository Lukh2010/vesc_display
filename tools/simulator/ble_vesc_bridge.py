#!/usr/bin/env python3
import socket
import select
import sys
import time
import threading
import dbus
from gi.repository import GLib
from dbus.mainloop.glib import DBusGMainLoop

BLE_DEV_PATH = '/org/bluez/hci0/dev_DB_AE_D9_B8_CB_46'
CHAR_TX_PATH = '/org/bluez/hci0/dev_DB_AE_D9_B8_CB_46/service0009/char000d' # Write to VESC
CHAR_RX_PATH = '/org/bluez/hci0/dev_DB_AE_D9_B8_CB_46/service0009/char000a' # Notify from VESC
TCP_PORT = 6512

def main():
    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()

    # Ensure device is connected
    dev_obj = bus.get_object('org.bluez', BLE_DEV_PATH)
    dev_iface = dbus.Interface(dev_obj, 'org.bluez.Device1')
    dev_props = dbus.Interface(dev_obj, 'org.freedesktop.DBus.Properties')
    
    if not dev_props.Get('org.bluez.Device1', 'Connected'):
        print(f"[BLE_BRIDGE] Connecting to {BLE_DEV_PATH}...")
        dev_iface.Connect()
        time.sleep(1.0)
    
    print("[BLE_BRIDGE] Connected to VESC BLE!")

    tx_obj = bus.get_object('org.bluez', CHAR_TX_PATH)
    rx_obj = bus.get_object('org.bluez', CHAR_RX_PATH)
    tx_char = dbus.Interface(tx_obj, 'org.bluez.GattCharacteristic1')
    rx_char = dbus.Interface(rx_obj, 'org.bluez.GattCharacteristic1')

    # Start notification
    rx_char.StartNotify()
    print("[BLE_BRIDGE] Notifications enabled on Nordic UART RX")

    # TCP Server
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', TCP_PORT))
    srv.listen(1)
    srv.setblocking(False)
    print(f"[BLE_BRIDGE] Listening on 127.0.0.1:{TCP_PORT} for VESC Tool connections...")

    active_client = None
    client_lock = threading.Lock()

    def on_rx_notify(interface, changed, invalidated):
        nonlocal active_client
        if 'Value' in changed:
            val = bytes(changed['Value'])
            with client_lock:
                if active_client:
                    try:
                        active_client.sendall(val)
                    except Exception as e:
                        active_client = None

    bus.add_signal_receiver(on_rx_notify,
                            dbus_interface='org.freedesktop.DBus.Properties',
                            signal_name='PropertiesChanged',
                            path=CHAR_RX_PATH)

    # Thread to handle TCP client read & write to BLE
    def tcp_worker():
        nonlocal active_client
        while True:
            try:
                # Accept new client if none
                with client_lock:
                    cur = active_client

                if cur is None:
                    try:
                        r, _, _ = select.select([srv], [], [], 0.2)
                        if r:
                            cli, addr = srv.accept()
                            cli.setblocking(True)
                            with client_lock:
                                active_client = cli
                            print(f"[BLE_BRIDGE] VESC Tool connected from {addr}!")
                    except Exception:
                        pass
                else:
                    # Read from client
                    r, _, _ = select.select([cur], [], [], 0.05)
                    if r:
                        data = cur.recv(4096)
                        if not data:
                            print("[BLE_BRIDGE] VESC Tool disconnected.")
                            with client_lock:
                                active_client = None
                        else:
                            # Send to BLE in MTU chunks (max ~128 bytes per BLE packet)
                            chunk_size = 128
                            for i in range(0, len(data), chunk_size):
                                chunk = data[i:i+chunk_size]
                                try:
                                    tx_char.WriteValue(list(chunk), {'type': 'command'})
                                except Exception:
                                    try:
                                        tx_char.WriteValue(list(chunk), {})
                                    except Exception as e:
                                        print("[BLE_BRIDGE] BLE Write error:", e)
                                time.sleep(0.005)
            except Exception as e:
                time.sleep(0.1)

    t = threading.Thread(target=tcp_worker, daemon=True)
    t.start()

    loop = GLib.MainLoop()
    try:
        loop.run()
    except KeyboardInterrupt:
        pass
    finally:
        try:
            rx_char.StopNotify()
        except:
            pass

if __name__ == '__main__':
    main()
