import socket
from .base_client import BaseClient

class UDPClient(BaseClient):
    def __init__(self, device_ip, device_port, local_ip="", local_port=0, timeout=2.0):
        self.device_ip = device_ip
        self.device_port = device_port
        self.local_ip = local_ip
        self.local_port = local_port
        self.timeout = timeout
        self.socket = None
        self._connected = False

    def connect(self):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.socket.settimeout(self.timeout)
        if self.local_ip:
            self.socket.bind((self.local_ip, self.local_port))
        self._connected = True

    def disconnect(self):
        if self.socket:
            self.socket.close()
            self.socket = None
        self._connected = False

    def send(self, data: bytes):
        if not self._connected or not self.socket:
            raise ConnectionError("UDP Client is not connected.")
        self.socket.sendto(data, (self.device_ip, self.device_port))

    def receive(self, length: int) -> bytes:
        if not self._connected or not self.socket:
            raise ConnectionError("UDP Client is not connected.")

        # In UDP, typically we receive the whole datagram up to 'length' size
        data, addr = self.socket.recvfrom(length)
        return data

    def is_connected(self) -> bool:
        return self._connected
