import time
from datetime import datetime
from PySide6.QtCore import QThread, Signal
from ..communication.tcp_client import TCPClient
from ..communication.udp_client import UDPClient
from ..communication.mock_client import MockClient
from ..protocol.fawave_protocol import FaWaveProtocol, ProtocolError

class AcquisitionWorker(QThread):
    # Signals for UI updates
    data_received = Signal(float, int, dict) # rel_time, sample_index, data_dict
    error_occurred = Signal(str)
    connection_status_changed = Signal(str) # "Connected", "Disconnected", "Error"
    stats_updated = Signal(int, int) # recv_frames, error_frames

    def __init__(self, config, data_buffer, data_recorder, logger):
        super().__init__()
        self.config = config
        self.data_buffer = data_buffer
        self.data_recorder = data_recorder
        self.logger = logger
        self.protocol = FaWaveProtocol(self.config)

        self.client = None
        self.is_running = False

        self.request_interval = self.config.get("request_interval_ms", 20) / 1000.0
        self.frame_length = self.config.get("frame_length", 35)

        # Load force decoder and alarm manager if available (stubs for now)
        try:
            from ..safety.safety_monitor import SafetyMonitor
            self.safety_monitor = SafetyMonitor(self.config)
        except ImportError:
            self.safety_monitor = None

        self.init_decoder()

        self.input_scale_to_v = self.config.get("force_decoder", {}).get("input_scale_to_v", 0.001)
        self.calibration_name = self.config.get("force_decoder", {}).get("calibration", {}).get("name", "未配置")
        self.calibration_version = self.config.get("force_decoder", {}).get("calibration", {}).get("version", "未知")
        self.calibration_date = self.config.get("force_decoder", {}).get("calibration", {}).get("date", "未配置")
        self.algorithm = self.config.get("force_decoder", {}).get("algorithm", "python_force_decoder")

    def set_task_mode(self, mode):
        if hasattr(self, 'safety_monitor') and self.safety_monitor:
            self.safety_monitor.set_task_mode(mode)

    def init_decoder(self):
        try:
            from ..force.force_decoder import ForceDecoder
            self.force_decoder = ForceDecoder(self.config)
            self.decoder_backend_str = "Python解耦"
        except Exception as e:
            self.logger.error(f"Failed to load ForceDecoder: {e}")
            self.force_decoder = None
            self.decoder_backend_str = "加载失败"

        self.recv_frames = 0
        self.error_frames = 0
        self.consecutive_errors = 0
        self.max_consecutive_errors = 20
        self.sample_index = 0
        self.start_time = 0

        self.zero_requested = False

    def set_connection_params(self, mode, ip, port, local_ip=""):
        self.mode = mode
        if mode == "TCP":
            self.client = TCPClient(ip, port)
        elif mode == "UDP":
            self.client = UDPClient(ip, port, local_ip)
        elif mode == "Mock":
            self.client = MockClient(self.config)
        else:
            raise ValueError(f"Unknown communication mode: {mode}")

    def run(self):
        self.is_running = True
        self.recv_frames = 0
        self.error_frames = 0
        self.consecutive_errors = 0
        self.sample_index = 0

        try:
            self.client.connect()
            self.logger.info("连接成功")
            self.connection_status_changed.emit("Connected")
            self.start_time = time.time()

            # Reset the ForceDecoder each time a connection is established to clear any previous warmups/baselines
            if self.force_decoder:
                if hasattr(self.force_decoder, 'reset'):
                    self.force_decoder.reset()
                elif hasattr(self.force_decoder, 'initialized'):
                    self.force_decoder.initialized = False

        except Exception as e:
            err_msg = str(e).lower()
            if "timed out" in err_msg or "timeout" in err_msg:
                self.logger.error("连接失败: timeout，可能原因包括设备未供电、网线未连接或端口未打开。")
                self.error_occurred.emit("连接失败：请检查设备电源、IP 地址、端口号和网线连接。")
            else:
                self.logger.error(f"连接失败: {str(e)}", exc_info=True)
                self.error_occurred.emit(f"连接失败: TCP 连接失败 ({str(e)})")
            self.connection_status_changed.emit("Error")
            self.is_running = False
            return

        request_frame = self.protocol.build_request_frame()

        while self.is_running:
            loop_start = time.time()

            try:
                if self.zero_requested:
                    if self.force_decoder and getattr(self.force_decoder, 'initialized', False):
                        self.force_decoder.set_baseline(None)
                        self.logger.info("用户执行三维力归零")
                    self.zero_requested = False

                # 1. Send request
                self.client.send(request_frame)

                # 2. Receive response
                # TCP Client now uses receive_frame_sync under the hood for `receive`
                response_frame = self.client.receive(self.frame_length)

                # 3. Parse response
                data_dict = self.protocol.parse_response_frame(response_frame)

                # Decode forces if decoder is available
                if self.force_decoder:
                    ch_v = [
                        data_dict.get("ch1", 0.0) * self.input_scale_to_v,
                        data_dict.get("ch2", 0.0) * self.input_scale_to_v,
                        data_dict.get("ch3", 0.0) * self.input_scale_to_v,
                        data_dict.get("ch4", 0.0) * self.input_scale_to_v
                    ]

                    if not getattr(self.force_decoder, 'initialized', False):
                        self.logger.info(f"ForceDecoder 输入单位: V")
                        self.logger.info(f"InputScaleToV: 1.0")
                        self.logger.info(f"解耦矩阵版本: {self.calibration_version}")
                        self.logger.info(f"解耦算法: {self.decoder_backend_str}")
                        self.force_decoder.initialize(ch_v)

                    rel_time_ms = int((time.time() - self.start_time) * 1000)
                    prev_startup_status = self.force_decoder.startup_baseline_done

                    try:
                        force_res = self.force_decoder.update(ch_v, rel_time_ms)
                    except Exception as e:
                        self.logger.error(f"解耦异常: {e}", exc_info=True)
                        force_res = { "fx": 0.0, "fy": 0.0, "fz": 0.0, "status": "错误", "valid": False }

                    if not prev_startup_status and self.force_decoder.startup_baseline_done:
                        self.logger.info("开机基线建立完成。")

                    data_dict["fx"] = force_res.get("fx", 0.0)
                    data_dict["fy"] = force_res.get("fy", 0.0)
                    data_dict["fz"] = force_res.get("fz", 0.0)
                    data_dict["fx_raw"] = force_res.get("fx_raw", 0.0)
                    data_dict["fy_raw"] = force_res.get("fy_raw", 0.0)
                    data_dict["fz_raw"] = force_res.get("fz_raw", 0.0)
                    data_dict["fx_filtered"] = force_res.get("fx_filtered", 0.0)
                    data_dict["fy_filtered"] = force_res.get("fy_filtered", 0.0)
                    data_dict["fz_filtered"] = force_res.get("fz_filtered", 0.0)
                    data_dict["d"] = force_res.get("d", [0.0]*4)
                    data_dict["baseline"] = force_res.get("baseline", [0.0]*4)

                    data_dict["decoder_status"] = force_res.get("status", "未启用")
                    data_dict["decoder_valid"] = force_res.get("valid", False)
                    data_dict["Algorithm"] = getattr(self, "decoder_backend_str", "未配置")
                    data_dict["input_unit"] = self.config.get("force_decoder", {}).get("input_unit", "V")
                    data_dict["input_scale_to_v"] = self.input_scale_to_v
                    data_dict["calibration_name"] = self.calibration_name
                    data_dict["calibration_version"] = self.calibration_version
                    data_dict["calibration_date"] = self.calibration_date
                    data_dict["acquisition_mode"] = "真实设备" if self.mode == "TCP" else "仿真演示"

                    if self.safety_monitor:
                        alarms = self.safety_monitor.update(
                            force_res.get("fx", 0.0),
                            force_res.get("fy", 0.0),
                            force_res.get("fz", 0.0),
                            rel_time_ms,
                            force_res.get("status", "未启用")
                        )
                        data_dict["alarms"] = alarms
                        recent = self.safety_monitor.get_recent_alarm()
                        data_dict["recent_alarm"] = recent
                        data_dict["task_mode"] = self.safety_monitor.mode
                    else:
                        data_dict["task_mode"] = "牵拉模式"

                # Reset error counter on success
                self.consecutive_errors = 0
                self.recv_frames += 1
                self.sample_index += 1

                abs_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
                rel_time = time.time() - self.start_time

                # 4. Add to buffer
                self.data_buffer.add_point(
                    rel_time,
                    self.sample_index,
                    data_dict.get("ch1", 0.0),
                    data_dict.get("ch2", 0.0),
                    data_dict.get("ch3", 0.0),
                    data_dict.get("ch4", 0.0),
                    data_dict.get("fx", 0.0),
                    data_dict.get("fy", 0.0),
                    data_dict.get("fz", 0.0),
                    data_dict.get("decoder_status", "未启用"),
                    data_dict.get("decoder_backend", "未配置"),
                    data_dict.get("decoder_validated", "未验证"),
                    alarms=data_dict.get("alarms"),
                    recent_alarm=data_dict.get("recent_alarm")
                )

                # 5. Record if enabled
                self.data_recorder.record_point(
                    abs_time, rel_time, self.sample_index, data_dict, data_dict.get("raw_hex", ""), "OK", data_dict.get("trailer_hex", "")
                )

                # Emit data for the UI (UI can choose to use this directly or read from buffer via QTimer)
                self.data_received.emit(rel_time, self.sample_index, data_dict)
                self.stats_updated.emit(self.recv_frames, self.error_frames)

            except ProtocolError as e:
                self.error_frames += 1
                self.consecutive_errors += 1

                err_msg = str(e)
                if "length" in err_msg.lower():
                    self.error_occurred.emit(f"最近错误：帧长度错误，期望 {self.frame_length} 字节")
                elif "header" in err_msg.lower():
                    self.error_occurred.emit("最近错误：帧头错误，期望 5A A5")
                elif "float" in err_msg.lower():
                    self.error_occurred.emit("最近错误：数据解析错误")
                else:
                    self.error_occurred.emit(f"最近错误：{err_msg}")

                # Still record the error frame if recording
                if hasattr(self, 'data_recorder') and self.data_recorder.is_recording:
                    abs_time = datetime.now().strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]
                    rel_time = time.time() - self.start_time
                    self.data_recorder.record_point(abs_time, rel_time, self.sample_index, {}, "", "ProtocolError")

                if self.consecutive_errors >= self.max_consecutive_errors:
                    self.logger.error("断开原因: 连续协议错误过多")
                    self.error_occurred.emit("最近错误：连续错误过多，断开连接")
                    self.connection_status_changed.emit("Error")
                    break

            except ConnectionError as e:
                err_msg = str(e).lower()
                if "timeout" in err_msg:
                    self.logger.error("协议错误: 接收超时 (设备可能已断电或断开连接)", exc_info=True)
                    self.error_occurred.emit("最近错误：接收超时，请检查设备连接或电源")
                elif "broken" in err_msg or "closed" in err_msg:
                    self.logger.error("断开原因: 远程主机关闭连接", exc_info=True)
                    self.error_occurred.emit("最近错误：远程主机关闭连接")
                else:
                    self.logger.error(f"断开原因: TCP 连接失败 ({str(e)})", exc_info=True)
                    self.error_occurred.emit(f"最近错误：TCP 连接失败 ({str(e)})")

                self.connection_status_changed.emit("Error")
                break
            except Exception as e:
                self.logger.error(f"异常堆栈: {str(e)}", exc_info=True)
                self.error_occurred.emit(f"最近错误：{str(e)}")
                self.connection_status_changed.emit("Error")
                break

            # 6. Wait for next interval
            elapsed = time.time() - loop_start
            sleep_time = self.request_interval - elapsed
            if sleep_time > 0:
                time.sleep(sleep_time)

        # Cleanup
        if self.client:
            try:
                self.client.disconnect()
            except:
                pass

        self.connection_status_changed.emit("Disconnected")

    def request_force_zero(self):
        self.zero_requested = True

    def stop(self):
        self.is_running = False
        self.wait() # wait for thread to finish safely
