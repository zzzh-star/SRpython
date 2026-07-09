import socket
from .base_client import BaseClient

class TCPClient(BaseClient):
    def __init__(self, ip, port, timeout=2.0):
        self.ip = ip
        self.port = port
        self.timeout = timeout
        self.socket = None
        self._connected = False

    def connect(self):
        self.socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.socket.settimeout(self.timeout)
        self.socket.connect((self.ip, self.port))
        self._connected = True

    def disconnect(self):
        if self.socket:
            self.socket.close()
            self.socket = None
        self._connected = False

    def send(self, data: bytes):
        if not self._connected or not self.socket:
            raise ConnectionError("TCP Client is not connected.")
        self.socket.sendall(data)

    def receive(self, length: int) -> bytes:
        return self.receive_frame_sync(length, b'\x5A\xA5')

    def receive_frame_sync(self, frame_length: int, header: bytes) -> bytes:
        if not self._connected or not self.socket:
            raise ConnectionError("TCP Client is not connected.")

        if not hasattr(self, '_buffer'):
            self._buffer = bytearray()

        header_len = len(header)

        while True:
            # Check if we have enough data to search for header
            if len(self._buffer) >= header_len:
                idx = self._buffer.find(header)
                if idx != -1:
                    # Discard garbage before the header
                    if idx > 0:
                        del self._buffer[:idx]

                    # Check if we have a full frame
                    if len(self._buffer) >= frame_length:
                        frame = bytes(self._buffer[:frame_length])
                        del self._buffer[:frame_length]
                        return frame
                else:
                    # Header not found, keep the last len(header)-1 bytes in case the header is split
                    del self._buffer[:-header_len + 1]

            # Receive more data
            try:
                chunk = self.socket.recv(4096)
                if chunk == b'':
                    raise ConnectionError("Socket connection broken")
                self._buffer.extend(chunk)
            except socket.timeout:
                raise ConnectionError("Socket receive timeout")

    def is_connected(self) -> bool:
        return self._connected
