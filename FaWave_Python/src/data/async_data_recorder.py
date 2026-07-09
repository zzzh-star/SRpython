import os
import csv
import threading
import queue
import time
import pandas as pd

class AsyncDataRecorder:
    def __init__(self, config=None):
        storage_cfg = (config or {}).get("storage", {})
        self.flush_interval_s = float(storage_cfg.get("flush_interval_s", 1.0))
        self.batch_size = int(storage_cfg.get("batch_size", 200))
        self.queue_max_points = int(storage_cfg.get("queue_max_points", 50000))
        self.auto_split_interval_s = float(storage_cfg.get("auto_split_interval_s", 600))
        self.max_rows_per_file = int(storage_cfg.get("max_rows_per_file", 300000))

        self.file_path = None
        self.file_format = None
        self.is_recording = False
        self.state = "idle" # idle, file_prepared, recording, stopping, stopped, error
        self.last_error = ""
        self.file_path = ""
        self.file_format = ""

        self.queue = queue.Queue(maxsize=self.queue_max_points)
        self._writer_thread = None

        self._csv_file = None
        self._csv_writer = None
        self.csv_headers = [
            "AbsoluteTime", "RelativeTime_s", "SampleIndex",
            "AcquisitionMode",
            "CH1_V", "CH2_V", "CH3_V", "CH4_V",
            "Fx_N", "Fy_N", "Fz_N",
            "FxRaw_N", "FyRaw_N", "FzRaw_N",
            "FxFiltered_N", "FyFiltered_N", "FzFiltered_N",
            "D1_V", "D2_V", "D3_V", "D4_V",
            "Baseline1_V", "Baseline2_V", "Baseline3_V", "Baseline4_V",
            "DecoderStatus", "DecoderValid", "Algorithm", "InputUnit", "InputScaleToV",
            "CalibrationName", "CalibrationVersion", "CalibrationDate",
            "TaskMode", "TaskState", "AlarmEvent", "AlarmLevel", "AlarmReason", "AlarmTriggered", "AlarmTimestamp_ms",
            "RawHex", "TrailerHex", "Status"
        ]

        self.queued_count = 0
        self.saved_count = 0
        self.dropped_count = 0
        self.started_at = 0
        self.stopped_at = 0
        self.segment_index = 1
        self.rows_in_current_file = 0
        self.current_file_started_at = 0
        self.current_output_path = ""
        self.output_paths = []

    def prepare_file(self, file_path, format="CSV"):
        """Creates the file and writes headers immediately, but does not start the writer loop."""
        if self.is_recording:
            return False

        self.file_path = file_path
        self.file_format = format.upper()

        self.queued_count = 0
        self.saved_count = 0
        self.queue = queue.Queue(maxsize=self.queue_max_points)
        self.dropped_count = 0
        self.segment_index = 1
        self.rows_in_current_file = 0
        self.current_file_started_at = time.time()
        self.current_output_path = self.file_path
        self.output_paths = []
        self.last_error = ""

        try:
            os.makedirs(os.path.dirname(os.path.abspath(self.file_path)), exist_ok=True)

            # Close existing if leaked
            if self._csv_file:
                try: self._csv_file.close()
                except: pass

            self._open_segment_writer()

            self.state = "file_prepared"
            return True
        except Exception as e:
            self.state = "error"
            self.last_error = str(e)
            return False

    def start_recording(self, file_path=None, format="CSV"):
        """Starts the background acquisition queueing."""
        if self.is_recording:
            return True

        if self.state != "file_prepared" or (file_path and self.file_path != file_path):
            success = self.prepare_file(file_path, format)
            if not success:
                return False

        self.is_recording = True
        self.state = "recording"
        self.started_at = time.time()

        # In a real edge case, the file might have been closed manually. Check.
        if not self._csv_file or self._csv_file.closed:
             mode = 'a'
             target = self.current_output_path if self.file_format == "CSV" else self._tmp_path
             self._csv_file = open(target, mode='a', newline='', encoding='utf-8')
             self._csv_writer = csv.writer(self._csv_file)

        self._writer_thread = threading.Thread(target=self._writer_loop, daemon=True)
        self._writer_thread.start()
        return True

    def stop_recording(self):
        if not self.is_recording and self.state != "recording":
            return

        self.is_recording = False
        self.state = "stopping"

        if self._writer_thread and self._writer_thread.is_alive():
            # Wait for thread to finish writing its queue safely without an arbitrary timeout
            self._writer_thread.join()

        if self._csv_file:
            try:
                self._csv_file.flush()
                self._csv_file.close()
            except:
                pass
            self._csv_file = None
            self._csv_writer = None

        if self.file_format == "XLSX":
            self._convert_temp_csv_to_xlsx()

        self.state = "stopped"
        self.stopped_at = time.time()

    def record_point(self, abs_time, rel_time, sample_idx, data_dict, raw_hex="", status="OK", trailer_hex=""):
        if not self.is_recording:
            return

        recent_alarm = data_dict.get("recent_alarm", None)
        if recent_alarm:
             alarm_event = recent_alarm.get("event", "")
             alarm_level = recent_alarm.get("level", "")
             alarm_reason = recent_alarm.get("reason", "")
             alarm_ts = recent_alarm.get("ts", 0)
             alarm_triggered = True
        else:
             alarm_event = ""
             alarm_level = ""
             alarm_reason = ""
             alarm_ts = 0
             alarm_triggered = False

        d = data_dict.get("d", [0.0]*4)
        baseline = data_dict.get("baseline", [0.0]*4)

        row = [
            abs_time,
            f"{rel_time:.3f}",
            sample_idx,
            data_dict.get("acquisition_mode", "未知"),
            data_dict.get("ch1", 0.0),
            data_dict.get("ch2", 0.0),
            data_dict.get("ch3", 0.0),
            data_dict.get("ch4", 0.0),
            data_dict.get("fx", 0.0),
            data_dict.get("fy", 0.0),
            data_dict.get("fz", 0.0),
            data_dict.get("fx_raw", 0.0),
            data_dict.get("fy_raw", 0.0),
            data_dict.get("fz_raw", 0.0),
            data_dict.get("fx_filtered", 0.0),
            data_dict.get("fy_filtered", 0.0),
            data_dict.get("fz_filtered", 0.0),
            d[0], d[1], d[2], d[3],
            baseline[0], baseline[1], baseline[2], baseline[3],
            data_dict.get("decoder_status", "未启用"),
            data_dict.get("decoder_valid", False),
            data_dict.get("Algorithm", "未配置"),
            data_dict.get("input_unit", "V"),
            data_dict.get("input_scale_to_v", 1.0),
            data_dict.get("calibration_name", "未配置"),
            data_dict.get("calibration_version", "未知"),
            data_dict.get("calibration_date", "未配置"),
            data_dict.get("task_mode", ""),
            data_dict.get("task_state", ""),
            alarm_event,
            alarm_level,
            alarm_reason,
            alarm_triggered,
            alarm_ts,
            raw_hex,
            trailer_hex,
            status
        ]

        try:
            self.queue.put_nowait(row)
            self.queued_count += 1
        except queue.Full:
            self.dropped_count += 1
            self.last_error = "保存队列已满，已丢弃部分数据；请缩短分段时间或检查磁盘写入速度"

    def _writer_loop(self):
        batch = []
        last_flush_time = time.time()

        while self.is_recording or not self.queue.empty():
            try:
                row = self.queue.get(timeout=0.1)
                batch.append(row)
                self.queued_count -= 1
            except queue.Empty:
                pass

            current_time = time.time()
            # If batch gets too big, or it's been long enough AND we have items, flush.
            # On shutdown (not is_recording), also flush remaining items.
            if len(batch) >= self.batch_size or (batch and (current_time - last_flush_time) >= self.flush_interval_s) or (not self.is_recording and batch):
                if self._csv_writer:
                    self._csv_writer.writerows(batch)
                    self._csv_file.flush()
                    self.saved_count += len(batch)
                    self.rows_in_current_file += len(batch)
                    batch.clear()
                    self._rotate_segment_if_needed()
                last_flush_time = current_time

    def _segment_path(self, index):
        if index <= 1:
            return self.file_path
        root, ext = os.path.splitext(self.file_path)
        return f"{root}_part{index:03d}{ext}"

    def _open_segment_writer(self):
        self.current_output_path = self._segment_path(self.segment_index)
        self.current_file_started_at = time.time()
        self.rows_in_current_file = 0

        if self.file_format == "CSV":
            target = self.current_output_path
        elif self.file_format == "XLSX":
            self._tmp_path = self.current_output_path + ".tmp.csv"
            target = self._tmp_path
        else:
            raise ValueError(f"Unsupported file format: {self.file_format}")

        self._csv_file = open(target, mode='w', newline='', encoding='utf-8')
        self._csv_writer = csv.writer(self._csv_file)
        self._csv_writer.writerow(self.csv_headers)
        self._csv_file.flush()
        self.output_paths.append(self.current_output_path)

    def _rotate_segment_if_needed(self):
        if not self.is_recording:
            return
        by_rows = self.max_rows_per_file > 0 and self.rows_in_current_file >= self.max_rows_per_file
        by_time = self.auto_split_interval_s > 0 and (time.time() - self.current_file_started_at) >= self.auto_split_interval_s
        if not (by_rows or by_time):
            return

        if self._csv_file:
            self._csv_file.flush()
            self._csv_file.close()
            self._csv_file = None
            self._csv_writer = None

        if self.file_format == "XLSX":
            self._convert_temp_csv_to_xlsx()

        self.segment_index += 1
        self._open_segment_writer()

    def _convert_temp_csv_to_xlsx(self):
        if not hasattr(self, '_tmp_path') or not os.path.exists(self._tmp_path):
            return

        try:
            df = pd.read_csv(self._tmp_path)
            df.to_excel(self.current_output_path, index=False)
            os.remove(self._tmp_path)
        except Exception as e:
            print(f"Failed to convert temp CSV to XLSX: {e}")

    def get_status(self):
        return {
            "state": self.state,
            "queued": self.queued_count,
            "saved": self.saved_count,
            "dropped": self.dropped_count,
            "file_path": self.file_path,
            "current_file": self.current_output_path,
            "segments": list(self.output_paths),
            "last_error": self.last_error
        }
