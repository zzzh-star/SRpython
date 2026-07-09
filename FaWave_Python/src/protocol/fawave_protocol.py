import struct
import binascii

class ProtocolError(Exception):
    """Exception raised for protocol parsing errors."""
    pass

class FaWaveProtocol:
    def __init__(self, config):
        self.frame_length = config.get("frame_length", 35)
        self.request_hex = config.get("request_frame_hex", "5A A5 82 01 0D 00 00 00 00 00 00 FF FF")

        protocol_config = config.get("protocol", {})
        self.header_hex = protocol_config.get("header_hex", "5AA5")
        self.header_bytes = bytes.fromhex(self.header_hex)
        self.float_endian = protocol_config.get("float_endian", "<")
        self.channels = protocol_config.get("channels", 4)
        self.data_offset = protocol_config.get("data_offset", 5)
        self.trailer_offset = protocol_config.get("trailer_offset", 21)

    def build_request_frame(self) -> bytes:
        """
        Build the request frame from the configured HEX string.
        """
        hex_str = self.request_hex.replace(" ", "")
        return bytes.fromhex(hex_str)

    def parse_response_frame(self, frame: bytes) -> dict:
        """
        Parse the response frame to extract channel data.

        Args:
            frame: The raw bytes received.

        Returns:
            A dictionary containing channel data, e.g., {"ch1": float, ...}

        Raises:
            ProtocolError if length, header, or parsing fails.
        """
        if len(frame) != self.frame_length:
            raise ProtocolError(f"Invalid frame length: expected {self.frame_length}, got {len(frame)}")

        if not frame.startswith(self.header_bytes):
            expected = self.header_bytes.hex().upper()
            got = frame[:len(self.header_bytes)].hex().upper()
            raise ProtocolError(f"Invalid frame header: expected {expected}, got {got}")

        data = {}
        offset = self.data_offset
        float_size = 4  # 4 bytes for a float32

        # Format string for struct.unpack: e.g., '<f' for little-endian float
        fmt = self.float_endian + 'f'

        try:
            for i in range(1, self.channels + 1):
                chunk = frame[offset : offset + float_size]
                if len(chunk) != float_size:
                    raise ProtocolError(f"Insufficient data for channel {i}")

                # Unpack returns a tuple, get the first element
                value = struct.unpack(fmt, chunk)[0]
                data[f"ch{i}"] = value
                offset += float_size
        except struct.error as e:
            raise ProtocolError(f"Float parsing error: {e}")

        # Keep raw hex for debugging
        data["raw_hex"] = frame.hex().upper()
        if len(frame) >= self.trailer_offset:
            data["trailer_hex"] = frame[self.trailer_offset:].hex().upper()
        else:
            data["trailer_hex"] = ""

        return data
