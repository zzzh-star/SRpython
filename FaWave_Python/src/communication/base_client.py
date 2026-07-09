from abc import ABC, abstractmethod

class BaseClient(ABC):
    @abstractmethod
    def connect(self):
        """Connect to the device."""
        pass

    @abstractmethod
    def disconnect(self):
        """Disconnect from the device."""
        pass

    @abstractmethod
    def send(self, data: bytes):
        """Send data to the device."""
        pass

    @abstractmethod
    def receive(self, length: int) -> bytes:
        """Receive data from the device."""
        pass

    @abstractmethod
    def is_connected(self) -> bool:
        """Return True if connected, False otherwise."""
        pass
