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

class BleVescBridge:
    def __init__(self):
        DBusGMainLoop(set_as_default=True)
        self.bus = dbus.SystemBus()
        self.dev_obj = self.bus.get_object('org.bluez', BLE_DEV_PATH)
        self.dev_iface = dbus.Interface(self.dev_obj, 'org.bluez.Device1')
        self.dev_props = dbus.Interface(self.dev_obj, 'org.freedesktop.DBus.Properties')
        
        self.tx_char = None
        self.rx_char = None
        self.ble_connected = False
        
        self.active_client = None
        self.client_lock = threading.Lock()
        
        self.srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(('127.0.0.1', TCP_PORT))
        self.srv.listen(1)
        self.srv.setblocking(False)
        print(f"[BLE_BRIDGE] Server listening on 127.0.0.1:{TCP_PORT}")

    def ensure_ble_connected(self):
        try:
            is_conn = bool(self.dev_props.Get('org.bluez.Device1', 'Connected'))
        except Exception:
            is_conn = False

        if not is_conn:
            self.ble_connected = False
            try:
                print("[BLE_BRIDGE] Connecting to VESC BLE...")
                self.dev_iface.Connect()
                time.sleep(1.0)
            except Exception as e:
                # If error, maybe needs scan filter refresh
                return False

        if not self.ble_connected:
            try:
                tx_obj = self.bus.get_object('org.bluez', CHAR_TX_PATH)
                rx_obj = self.bus.get_object('org.bluez', CHAR_RX_PATH)
                self.tx_char = dbus.Interface(tx_obj, 'org.bluez.GattCharacteristic1')
                self.rx_char = dbus.Interface(rx_obj, 'org.bluez.GattCharacteristic1')
                self.rx_char.StartNotify()
                self.ble_connected = True
                print("[BLE_BRIDGE] Connected to VESC BLE & notifications started!")
            except Exception as e:
                return False
        return True

    def on_rx_notify(self, interface, changed, invalidated):
        if 'Value' in changed:
            val = bytes(changed['Value'])
            with self.client_lock:
                if self.active_client:
                    try:
                        self.active_client.sendall(val)
                    except Exception:
                        self.active_client = None

    def on_dev_props_changed(self, interface, changed, invalidated):
        if 'Connected' in changed:
            conn = bool(changed['Connected'])
            print(f"[BLE_BRIDGE] BLE Connection state changed: {conn}")
            self.ble_connected = conn

    def run_tcp_loop(self):
        while True:
            try:
                # 1. Maintain BLE connection
                if not self.ble_connected:
                    self.ensure_ble_connected()
                    time.sleep(1.0)
                    continue

                # 2. Handle TCP client
                with self.client_lock:
                    cur = self.active_client

                if cur is None:
                    try:
                        r, _, _ = select.select([self.srv], [], [], 0.2)
                        if r:
                            cli, addr = self.srv.accept()
                            cli.setblocking(True)
                            with self.client_lock:
                                self.active_client = cli
                            print(f"[BLE_BRIDGE] VESC Tool connected from {addr}!")
                    except Exception:
                        pass
                else:
                    r, _, _ = select.select([cur], [], [], 0.05)
                    if r:
                        data = cur.recv(4096)
                        if not data:
                            print("[BLE_BRIDGE] VESC Tool disconnected.")
                            with self.client_lock:
                                self.active_client = None
                        else:
                            # Forward to BLE
                            chunk_size = 128
                            for i in range(0, len(data), chunk_size):
                                chunk = data[i:i+chunk_size]
                                try:
                                    if self.tx_char:
                                        self.tx_char.WriteValue(list(chunk), {'type': 'command'})
                                except Exception:
                                    try:
                                        if self.tx_char:
                                            self.tx_char.WriteValue(list(chunk), {})
                                    except Exception as e:
                                        print("[BLE_BRIDGE] BLE Write error:", e)
                                        self.ble_connected = False
                                time.sleep(0.005)
            except Exception as e:
                time.sleep(0.1)

    def start(self):
        self.bus.add_signal_receiver(self.on_rx_notify,
                                    dbus_interface='org.freedesktop.DBus.Properties',
                                    signal_name='PropertiesChanged',
                                    path=CHAR_RX_PATH)
        self.bus.add_signal_receiver(self.on_dev_props_changed,
                                    dbus_interface='org.freedesktop.DBus.Properties',
                                    signal_name='PropertiesChanged',
                                    path=BLE_DEV_PATH)

        self.ensure_ble_connected()

        t = threading.Thread(target=self.run_tcp_loop, daemon=True)
        t.start()

        loop = GLib.MainLoop()
        try:
            loop.run()
        except KeyboardInterrupt:
            pass

if __name__ == '__main__':
    bridge = BleVescBridge()
    bridge.start()
