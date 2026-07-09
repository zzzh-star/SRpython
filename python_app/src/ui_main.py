import time
import sys
import traceback
from pathlib import Path

from PySide6.QtCore import QTimer, Qt
from PySide6.QtGui import QFont, QFontDatabase, QFontMetrics, QGuiApplication
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QFormLayout,
    QFrame,
    QGridLayout,
    QGroupBox,
    QHBoxLayout,
    QLabel,
    QLineEdit,
    QMainWindow,
    QPlainTextEdit,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSpinBox,
    QSplitter,
    QTabWidget,
    QVBoxLayout,
    QWidget,
)
import pyqtgraph as pg

from .config_manager import load_config, save_config
from .cpp_bridge_client import CppBridgeClient
from .diagnostic_logger import DiagnosticLogger
from .experiment_logger import ExperimentLogger
from .force_processor import ForceProcessor
from .force_sensor import create_force_sensor
from .grasp_state_machine import GraspStateMachine
from .omega_feedback_policy import OmegaFeedbackPolicy
from .serial_tools import list_serial_ports
from .sma_controller import SMAController


def choose_ui_font_family() -> str:
    if sys.platform.startswith("win"):
        return "Microsoft YaHei UI"
    available = set(QFontDatabase.families())
    candidates = [
        "Microsoft YaHei UI",
        "Microsoft YaHei",
        "SimHei",
        "SimSun",
        "Arial Unicode MS",
        "Segoe UI",
        "Arial",
    ]
    for family in candidates:
        if family in available:
            return family
    return QApplication.font().family()


def apply_application_font():
    family = choose_ui_font_family()
    QApplication.setFont(QFont(family, 9))
    return family


class ValueCard(QFrame):
    def __init__(self, title: str, unit: str = "", compact: bool = False):
        super().__init__()
        self.setProperty("class", "card")
        self.compact = compact
        self.setMinimumSize(124, 68 if compact else 88)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)

        layout = QGridLayout(self)
        layout.setContentsMargins(10, 8, 10, 8)
        layout.setHorizontalSpacing(6)
        layout.setVerticalSpacing(2)

        title_label = QLabel(title)
        title_label.setProperty("class", "valueTitle")
        title_label.setTextInteractionFlags(Qt.NoTextInteraction)
        title_label.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)

        self.value_label = QLabel("0.000")
        self.value_label.setProperty("class", "valueNumber")
        self.value_label.setAlignment(Qt.AlignLeft | Qt.AlignVCenter)
        self.value_label.setMinimumWidth(0)
        self.value_label.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)

        self.unit_label = QLabel(unit)
        self.unit_label.setProperty("class", "valueTitle")
        self.unit_label.setAlignment(Qt.AlignLeft | Qt.AlignBottom)

        layout.addWidget(title_label, 0, 0, 1, 2)
        layout.addWidget(self.value_label, 1, 0)
        layout.addWidget(self.unit_label, 1, 1)
        layout.setColumnStretch(0, 1)

    def set_value(self, value, digits=3):
        if isinstance(value, str):
            text = value
        else:
            text = f"{value:.{digits}f}"
        metrics = QFontMetrics(self.value_label.font())
        available_width = max(40, self.value_label.width() - 4)
        self.value_label.setToolTip(text)
        self.value_label.setText(metrics.elidedText(text, Qt.ElideRight, available_width))


class SectionLabel(QLabel):
    def __init__(self, text: str):
        super().__init__(text)
        self.setProperty("class", "sectionLabel")
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)


def make_button(text: str, object_name: str = "") -> QPushButton:
    button = QPushButton(text)
    if object_name:
        button.setObjectName(object_name)
    button.setMinimumHeight(38)
    button.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
    return button


class CameraPreview(QFrame):
    def __init__(self):
        super().__init__()
        self.setObjectName("cameraPreview")
        self.setMinimumHeight(220)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        layout = QVBoxLayout(self)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(4)
        self.title = QLabel("摄像头画面")
        self.title.setProperty("class", "cameraTitle")
        self.title.setAlignment(Qt.AlignLeft | Qt.AlignTop)
        self.placeholder = QLabel("等待接入摄像头")
        self.placeholder.setProperty("class", "cameraPlaceholder")
        self.placeholder.setAlignment(Qt.AlignCenter)
        layout.addWidget(self.title)
        layout.addWidget(self.placeholder, 1)


class MainWindow(QMainWindow):
    def __init__(self, config_path: str):
        super().__init__()
        self.config_path = config_path
        self.config = load_config(config_path)
        self.diagnostics = DiagnosticLogger(config_path, self.config)
        self.diagnostics.snapshot("serial ports at startup", list_serial_ports())
        self.ui_font_family = QApplication.font().family()
        self.diagnostics.snapshot("ui font", {"family": self.ui_font_family})
        self.setWindowTitle("SR 主从协同控制平台")
        screen = QApplication.primaryScreen().availableGeometry()
        self.resize(min(1500, int(screen.width() * 0.9)), min(900, int(screen.height() * 0.9)))
        self.setMinimumSize(1240, 760)

        theme_path = Path(__file__).with_name("ui_theme.qss")
        with theme_path.open("r", encoding="utf-8") as f:
            self.setStyleSheet(f.read())

        self.bridge = None
        self.sensor = create_force_sensor(self.config["force_sensor"])
        self.processor = ForceProcessor(self.config["force_sensor"])
        self.state_machine = GraspStateMachine(self.config["grasp_state"])
        self.feedback_policy = OmegaFeedbackPolicy(self.config["omega_feedback"], self.config["grasp_state"])
        self.sma = SMAController(self.config["sma"])
        self.logger = ExperimentLogger(self.config["logger"])
        self.sensor_ok = False
        self.sma_ok = False
        self.recording = False
        self._last_temperature_query = 0.0
        self._last_sensor_error_log = 0.0
        self._last_snapshot_time = 0.0
        self._last_cpp_message_count = 0
        self._serial_ports = []
        self._serial_port_signature = ""

        self.sample_index = 0
        self.last_state = None
        self.fx_points = []
        self.fy_points = []
        self.fz_points = []
        self.t_points = []
        self.start_time = time.monotonic()

        self._build_ui()
        self._start_local_devices()

        self.timer = QTimer(self)
        self.timer.timeout.connect(self._tick)
        self.timer.start(int(1000 / self.config["omega_feedback"].get("send_rate_hz", 50)))

        self.port_timer = QTimer(self)
        self.port_timer.timeout.connect(self._auto_refresh_ports)
        self.port_timer.start(1500)

    def _build_ui(self):
        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(16, 16, 16, 10)
        root.setSpacing(14)

        header = QFrame()
        header.setObjectName("headerCard")
        header.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        header_layout = QHBoxLayout(header)
        header_layout.setContentsMargins(20, 14, 20, 14)
        header_layout.setSpacing(16)
        title_box = QVBoxLayout()
        title = QLabel("SR 主从协同控制平台")
        title.setObjectName("headerTitle")
        force_mode = "sim" if self.config["force_sensor"].get("simulation", True) else self.config["force_sensor"].get("port", "COM?")
        sma_mode = "sim" if self.config["sma"].get("simulation", True) else self.config["sma"].get("port", "COM?")
        subtitle = QLabel(f"Python 主控逻辑 | C++ Omega/Maxon 硬件桥 | 力传感器 {force_mode} | SMA {sma_mode}")
        subtitle.setObjectName("headerSubtitle")
        subtitle.setWordWrap(False)
        title_box.addWidget(title)
        title_box.addWidget(subtitle)
        header_layout.addLayout(title_box, 1)

        self.status = QLabel("C++ 未连接")
        self.status.setProperty("class", "statusCapsule")
        self.status.setObjectName("statusDisconnected")
        self.status.setMinimumWidth(150)
        self.status.setAlignment(Qt.AlignCenter)
        header_layout.addWidget(self.status)
        root.addWidget(header)

        self.tabs = QTabWidget()
        self.tabs.setObjectName("mainTabs")
        root.addWidget(self.tabs, 1)

        control_page = QWidget()
        control_layout = QVBoxLayout(control_page)
        control_layout.setContentsMargins(0, 0, 0, 0)
        control_layout.setSpacing(0)

        body = QSplitter(Qt.Horizontal)
        body.setObjectName("mainSplitter")
        body.setHandleWidth(5)
        control_layout.addWidget(body, 1)

        left_scroll = QScrollArea()
        left_scroll.setWidgetResizable(True)
        left_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        left_scroll.setMinimumWidth(270)
        left_scroll.setMaximumWidth(340)
        left = QFrame()
        left.setObjectName("leftPanel")
        left_layout = QVBoxLayout(left)
        left_layout.setContentsMargins(14, 14, 14, 14)
        left_layout.setSpacing(10)
        left_scroll.setWidget(left)

        left_layout.addWidget(SectionLabel("串口设置"))
        force_label = QLabel("从端力传感器串口")
        force_label.setProperty("class", "fieldLabel")
        self.combo_force_port = QComboBox()
        self.combo_force_port.setMinimumHeight(36)
        self.combo_force_port.setSizeAdjustPolicy(QComboBox.AdjustToMinimumContentsLengthWithIcon)
        self.combo_force_port.setMinimumContentsLength(22)
        self.combo_force_port.currentTextChanged.connect(lambda text: self._set_config_port("force_sensor", text))
        sma_label = QLabel("SMA 控制器串口")
        sma_label.setProperty("class", "fieldLabel")
        self.combo_sma_port = QComboBox()
        self.combo_sma_port.setMinimumHeight(36)
        self.combo_sma_port.setSizeAdjustPolicy(QComboBox.AdjustToMinimumContentsLengthWithIcon)
        self.combo_sma_port.setMinimumContentsLength(22)
        self.combo_sma_port.currentTextChanged.connect(lambda text: self._set_config_port("sma", text))
        left_layout.addWidget(force_label)
        left_layout.addWidget(self.combo_force_port)
        left_layout.addWidget(sma_label)
        left_layout.addWidget(self.combo_sma_port)

        left_layout.addSpacing(4)
        left_layout.addWidget(SectionLabel("主手与电机桥接"))
        self.btn_connect = make_button("连接 C++", "btnConnect")
        self.btn_connect.clicked.connect(self._connect_cpp)
        self.btn_start = make_button("开始实验", "btnStart")
        self.btn_start.clicked.connect(lambda: self._send_command("start_experiment"))
        self.btn_stop = make_button("停止实验")
        self.btn_stop.clicked.connect(lambda: self._send_command("stop_experiment"))
        self.btn_zero = make_button("主手清零")
        self.btn_zero.clicked.connect(lambda: self._send_command("zero_omega"))
        left_layout.addWidget(self.btn_connect)
        left_layout.addWidget(self.btn_start)
        left_layout.addWidget(self.btn_stop)
        left_layout.addWidget(self.btn_zero)

        left_layout.addSpacing(4)
        left_layout.addWidget(SectionLabel("硬件测试"))
        self.btn_reconnect_local = make_button("重连本地硬件")
        self.btn_reconnect_local.clicked.connect(self._reconnect_local_devices)
        self.btn_ports = make_button("查看串口列表")
        self.btn_ports.clicked.connect(self._list_com_ports)
        self.btn_sensor_test = make_button("测试力传感器")
        self.btn_sensor_test.clicked.connect(self._test_force_sensor_once)
        self.btn_sma_test = make_button("测试 SMA")
        self.btn_sma_test.clicked.connect(self._test_sma_once)
        for btn in [
            self.btn_reconnect_local,
            self.btn_ports,
            self.btn_sensor_test,
            self.btn_sma_test,
        ]:
            left_layout.addWidget(btn)

        left_layout.addSpacing(4)
        left_layout.addWidget(SectionLabel("数据与安全"))
        self.btn_record = make_button("开始记录")
        self.btn_record.clicked.connect(self._toggle_recording)
        self.btn_snapshot = make_button("保存诊断信息")
        self.btn_snapshot.clicked.connect(self._write_diagnostic_snapshot)
        self.btn_emergency = make_button("急停", "btnEmergency")
        self.btn_emergency.clicked.connect(self._emergency_stop)
        left_layout.addWidget(self.btn_record)
        left_layout.addWidget(self.btn_snapshot)
        left_layout.addWidget(self.btn_emergency)
        left_layout.addStretch(1)
        body.addWidget(left_scroll)

        center = QFrame()
        center.setObjectName("centerPanel")
        center.setMinimumWidth(520)
        center_layout = QVBoxLayout(center)
        center_layout.setContentsMargins(0, 0, 0, 0)
        center_layout.setSpacing(12)

        self.camera_preview = CameraPreview()
        center_layout.addWidget(self.camera_preview, 2)

        plot_card = QFrame()
        plot_card.setProperty("class", "card")
        plot_layout = QVBoxLayout(plot_card)
        plot_layout.setContentsMargins(14, 14, 14, 14)
        plot_layout.setSpacing(10)
        plot_title = QLabel("从端三维力曲线")
        plot_title.setProperty("class", "panelTitle")
        plot_layout.addWidget(plot_title)
        self.plot = pg.PlotWidget()
        self.plot.setMinimumHeight(260)
        self.plot.setBackground("#111827")
        self.plot.showGrid(x=True, y=True, alpha=0.25)
        self.plot.addLegend()
        axis_font = QFont(self.ui_font_family, 9)
        for axis_name in ("left", "bottom"):
            self.plot.getAxis(axis_name).setTickFont(axis_font)
            self.plot.getAxis(axis_name).setTextPen("#CBD5E1")
        self.fx_curve = self.plot.plot(pen=pg.mkPen("#3B82F6", width=2), name="Fx")
        self.fy_curve = self.plot.plot(pen=pg.mkPen("#F59E0B", width=2), name="Fy")
        self.fz_curve = self.plot.plot(pen=pg.mkPen("#22C55E", width=2), name="Fz")
        plot_layout.addWidget(self.plot, 1)
        center_layout.addWidget(plot_card, 3)
        body.addWidget(center)

        right = QFrame()
        right.setProperty("class", "card")
        right.setMinimumWidth(330)
        right.setMaximumWidth(430)
        right_layout = QVBoxLayout(right)
        right_layout.setContentsMargins(14, 14, 14, 14)
        right_layout.setSpacing(10)
        status_title = QLabel("运行状态")
        status_title.setProperty("class", "panelTitle")
        right_layout.addWidget(status_title)

        status_scroll = QScrollArea()
        status_scroll.setWidgetResizable(True)
        status_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        status_scroll.setMinimumHeight(260)
        status_scroll.setMaximumHeight(380)
        status_widget = QWidget()
        status_widget.setObjectName("statusScrollBody")
        status_widget_layout = QVBoxLayout(status_widget)
        status_widget_layout.setContentsMargins(0, 0, 0, 0)
        status_widget_layout.setSpacing(0)
        grid = QGridLayout()
        grid.setHorizontalSpacing(10)
        grid.setVerticalSpacing(10)
        self.cards = {
            "fx": ValueCard("Fx", "N", compact=True),
            "fy": ValueCard("Fy", "N", compact=True),
            "fz": ValueCard("Fz", "N", compact=True),
            "fn": ValueCard("法向力", "N", compact=True),
            "state": ValueCard("夹持状态", compact=True),
            "feedback": ValueCard("目标反馈", "N", compact=True),
            "applied": ValueCard("实际反馈", "N", compact=True),
            "omega": ValueCard("主手位置", compact=True),
            "sensor": ValueCard("力传感器", compact=True),
            "sma": ValueCard("SMA", compact=True),
            "sma_temp": ValueCard("SMA 温度", "C", compact=True),
            "recording": ValueCard("数据记录", compact=True),
        }
        for idx, card in enumerate(self.cards.values()):
            grid.addWidget(card, idx // 2, idx % 2)
        status_widget_layout.addLayout(grid)
        status_widget_layout.addStretch(1)
        status_scroll.setWidget(status_widget)
        right_layout.addWidget(status_scroll)

        log_title = QLabel("事件日志")
        log_title.setProperty("class", "panelTitle")
        right_layout.addWidget(log_title)
        self.log = QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMinimumHeight(190)
        self.log.setMaximumBlockCount(500)
        right_layout.addWidget(self.log, 1)
        body.addWidget(right)
        body.setSizes([300, 820, 370])
        self.tabs.addTab(control_page, "主控界面")
        self.tabs.addTab(self._build_parameter_page(), "参数设置")

        self._refresh_port_combos(log_result=False, force=True)

    def _set_config_port(self, section: str, combo_text: str):
        port = combo_text.split(" | ", 1)[0].strip()
        if port and port != "No COM":
            self.config[section]["port"] = port

    def _build_parameter_page(self):
        page_scroll = QScrollArea()
        page_scroll.setWidgetResizable(True)
        page_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)

        page = QWidget()
        layout = QVBoxLayout(page)
        layout.setContentsMargins(18, 18, 18, 18)
        layout.setSpacing(14)

        intro = QLabel("这里主要设置端口信息。主手 Omega、电机 Maxon、SMA 底层安全参数沿用原程序；实验逻辑参数后续需要时再逐步开放。")
        intro.setObjectName("paramIntro")
        intro.setWordWrap(True)
        layout.addWidget(intro)

        # 端口参数：这里是硬件测试最常改的部分。
        layout.addWidget(self._make_serial_param_group())
        layout.addWidget(self._make_reference_group(
            "主手与电机参数",
            "Omega 初始化、状态读取、力反馈输出、Maxon 电机使能和运动参数仍由原 C++ 程序负责。Python 只连接 C++ 硬件桥，不在这里修改底层驱动参数。"
        ))
        layout.addWidget(self._make_reference_group(
            "SMA 参数",
            "SMA 的 PWM、温度闭环、最大温度、急停等安全参数沿用 SMA程序.ino。Python 这里只选择串口并发送已有协议命令，例如 @H、@S、@T、x。"
        ))
        layout.addWidget(self._make_logger_param_group())

        button_row = QHBoxLayout()
        button_row.addStretch(1)
        apply_btn = make_button("应用参数")
        apply_btn.clicked.connect(self._apply_runtime_parameters)
        save_btn = make_button("保存到配置文件")
        save_btn.clicked.connect(self._save_parameters_to_file)
        button_row.addWidget(apply_btn)
        button_row.addWidget(save_btn)
        layout.addLayout(button_row)
        layout.addStretch(1)

        page_scroll.setWidget(page)
        return page_scroll

    def _make_group(self, title: str) -> tuple[QGroupBox, QFormLayout]:
        group = QGroupBox(title)
        form = QFormLayout(group)
        form.setContentsMargins(14, 18, 14, 14)
        form.setHorizontalSpacing(16)
        form.setVerticalSpacing(10)
        form.setLabelAlignment(Qt.AlignRight | Qt.AlignVCenter)
        return group, form

    def _make_reference_group(self, title: str, text: str):
        group = QGroupBox(title)
        layout = QVBoxLayout(group)
        layout.setContentsMargins(14, 18, 14, 14)
        label = QLabel(text)
        label.setWordWrap(True)
        label.setProperty("class", "paramHint")
        layout.addWidget(label)
        return group

    def _add_text_param(self, form: QFormLayout, label: str, section: str, key: str, tooltip: str = ""):
        edit = QLineEdit(str(self.config[section].get(key, "")))
        edit.setToolTip(tooltip)
        edit.textChanged.connect(lambda value, s=section, k=key: self._set_config_value(s, k, value))
        form.addRow(label, edit)
        return edit

    def _add_bool_param(self, form: QFormLayout, label: str, section: str, key: str, tooltip: str = ""):
        check = QCheckBox()
        check.setChecked(bool(self.config[section].get(key, False)))
        check.setToolTip(tooltip)
        check.stateChanged.connect(lambda _state, s=section, k=key, w=check: self._set_config_value(s, k, w.isChecked()))
        form.addRow(label, check)
        return check

    def _add_int_param(self, form: QFormLayout, label: str, section: str, key: str, min_value: int, max_value: int, tooltip: str = ""):
        spin = QSpinBox()
        spin.setRange(min_value, max_value)
        spin.setValue(int(self.config[section].get(key, min_value)))
        spin.setToolTip(tooltip)
        spin.valueChanged.connect(lambda value, s=section, k=key: self._set_config_value(s, k, int(value)))
        form.addRow(label, spin)
        return spin

    def _add_float_param(self, form: QFormLayout, label: str, section: str, key: str, min_value: float, max_value: float, decimals: int, step: float, tooltip: str = ""):
        spin = QDoubleSpinBox()
        spin.setRange(min_value, max_value)
        spin.setDecimals(decimals)
        spin.setSingleStep(step)
        spin.setValue(float(self.config[section].get(key, min_value)))
        spin.setToolTip(tooltip)
        spin.valueChanged.connect(lambda value, s=section, k=key: self._set_config_value(s, k, float(value)))
        form.addRow(label, spin)
        return spin

    def _set_config_value(self, section: str, key: str, value):
        self.config[section][key] = value

    def _make_serial_param_group(self):
        group, form = self._make_group("端口信息")
        self._add_text_param(form, "C++ 主机地址", "cpp_bridge", "host", "通常保持 127.0.0.1")
        self._add_int_param(form, "C++ 端口", "cpp_bridge", "port", 1, 65535, "C++ 硬件桥使用的 TCP 端口")
        self._add_text_param(form, "力传感器 COM", "force_sensor", "port", "也可以在主控界面下拉框中选择")
        self._add_int_param(form, "力传感器波特率", "force_sensor", "baud", 1200, 1000000)
        self._add_bool_param(form, "力传感器模拟模式", "force_sensor", "simulation", "勾选后不打开真实串口")
        self._add_text_param(form, "SMA COM", "sma", "port", "也可以在主控界面下拉框中选择")
        self._add_int_param(form, "SMA 波特率", "sma", "baud", 1200, 1000000)
        self._add_bool_param(form, "SMA 模拟模式", "sma", "simulation", "勾选后只打印命令，不打开真实串口")
        return group

    def _make_grasp_param_group(self):
        group, form = self._make_group("夹持状态判断")
        self._add_float_param(form, "接触阈值 F_contact", "grasp_state", "F_contact", 0.0, 10.0, 4, 0.001)
        self._add_float_param(form, "稳定下限 F_stable_low", "grasp_state", "F_stable_low", 0.0, 10.0, 4, 0.001)
        self._add_float_param(form, "稳定上限 F_stable_high", "grasp_state", "F_stable_high", 0.0, 10.0, 4, 0.001)
        self._add_float_param(form, "过载进入 F_over_enter", "grasp_state", "F_over_enter", 0.0, 10.0, 4, 0.001)
        self._add_float_param(form, "过载退出 F_over_exit", "grasp_state", "F_over_exit", 0.0, 10.0, 4, 0.001)
        self._add_int_param(form, "状态确认帧数", "grasp_state", "state_confirm_frames", 1, 200)
        self._add_int_param(form, "最小保持时间 ms", "grasp_state", "state_min_hold_ms", 0, 10000)
        return group

    def _make_feedback_param_group(self):
        group, form = self._make_group("Omega 反馈力策略")
        self._add_float_param(form, "普通反馈增益", "omega_feedback", "gain", 0.0, 1000.0, 2, 1.0)
        self._add_float_param(form, "过载反馈增益", "omega_feedback", "over_gain", 0.0, 1000.0, 2, 1.0)
        self._add_float_param(form, "反馈力限幅 N", "omega_feedback", "force_limit", 0.0, 50.0, 2, 0.5)
        self._add_float_param(form, "死区 deadband", "omega_feedback", "deadband", 0.0, 10.0, 3, 0.01)
        self._add_int_param(form, "发送频率 Hz", "omega_feedback", "send_rate_hz", 1, 1000)
        return group

    def _make_sma_param_group(self):
        group, form = self._make_group("SMA 温控策略")
        self._add_text_param(form, "SMA 通道 L/R/U/D/F", "sma", "channel", "L=左, R=右, U=上, D=下, F=全部")
        self._add_float_param(form, "报警温度上限 C", "sma", "alarm_temp", 30.0, 43.0, 1, 0.5)
        self._add_float_param(form, "接触目标温度 C", "sma", "temp_contact", 25.0, 40.0, 1, 0.5)
        self._add_float_param(form, "稳定目标温度 C", "sma", "temp_stable", 25.0, 40.0, 1, 0.5)
        self._add_float_param(form, "过载目标温度 C", "sma", "temp_over", 25.0, 40.0, 1, 0.5)
        self._add_int_param(form, "冷却时间 ms", "sma", "cooling_time_ms", 0, 60000)
        self._add_int_param(form, "过载重复间隔 ms", "sma", "over_repeat_interval_ms", 100, 60000)
        self._add_int_param(form, "温度查询间隔 ms", "sma", "temperature_query_interval_ms", 100, 60000)
        return group

    def _make_logger_param_group(self):
        group, form = self._make_group("日志与记录")
        self._add_text_param(form, "数据保存目录", "logger", "save_dir")
        self._add_bool_param(form, "自动保存 CSV", "logger", "auto_save")
        return group

    def _apply_runtime_parameters(self):
        # 应用运行时参数：重建依赖配置的 Python 逻辑对象。
        self.processor = ForceProcessor(self.config["force_sensor"])
        self.state_machine = GraspStateMachine(self.config["grasp_state"])
        self.feedback_policy = OmegaFeedbackPolicy(self.config["omega_feedback"], self.config["grasp_state"])
        self.sma.channel = str(self.config["sma"].get("channel", "L")).upper()
        interval = int(1000 / max(1, int(self.config["omega_feedback"].get("send_rate_hz", 50))))
        self.timer.setInterval(interval)
        self._refresh_port_combos(log_result=False, force=True)
        self._log("参数已应用。串口类参数如已修改，请点击 Reconnect Local 重新连接硬件。")
        self.diagnostics.snapshot("parameters applied", self.config)

    def _save_parameters_to_file(self):
        save_config(self.config_path, self.config)
        self._log(f"参数已保存到配置文件: {self.config_path}")
        self.diagnostics.snapshot("parameters saved", {"config_path": self.config_path, "config": self.config})

    def _format_port_label(self, device: str, description: str) -> str:
        if not description:
            return device
        description = description.replace(f"({device})", "").strip()
        max_desc_chars = 12
        short_desc = description if len(description) <= max_desc_chars else description[:max_desc_chars - 1] + "..."
        return f"{device} | {short_desc}"

    def _serial_signature(self, ports: list[dict]) -> str:
        return "|".join(
            f"{port.get('device', '')}:{port.get('description', '')}:{port.get('hwid', '')}"
            for port in ports
        )

    def _auto_refresh_ports(self):
        self._refresh_port_combos(log_result=True, force=False)

    def _refresh_port_combos(self, log_result: bool = True, force: bool = True):
        ports = list_serial_ports()
        signature = self._serial_signature(ports)
        if not force and signature == self._serial_port_signature:
            return

        previous_force = self.config["force_sensor"].get("port", "")
        previous_sma = self.config["sma"].get("port", "")
        self._serial_ports = ports
        self._serial_port_signature = signature
        self._populate_port_combo(self.combo_force_port, previous_force, self._serial_ports)
        self._populate_port_combo(self.combo_sma_port, previous_sma, self._serial_ports)
        self.diagnostics.snapshot("serial ports refreshed", self._serial_ports)
        if log_result:
            self._log(f"Serial ports changed: {len(self._serial_ports)} found.")
            self._warn_selected_ports()

    def _populate_port_combo(self, combo: QComboBox, selected_port: str, ports: list[dict]):
        combo.blockSignals(True)
        combo.clear()
        selected_port = str(selected_port).upper()
        has_selected = False
        for port in ports:
            if "error" in port:
                combo.addItem("Port list error")
                combo.setItemData(combo.count() - 1, str(port["error"]), Qt.ToolTipRole)
                continue
            device = str(port.get("device", ""))
            description = str(port.get("description", ""))
            if device.upper() == selected_port:
                has_selected = True
            combo.addItem(self._format_port_label(device, description))
            combo.setItemData(combo.count() - 1, f"{device} | {description} | {port.get('hwid', '')}", Qt.ToolTipRole)
        if selected_port and not has_selected:
            combo.insertItem(0, f"{selected_port} | not currently connected")
            combo.setItemData(0, f"{selected_port} is configured but not currently listed by Windows", Qt.ToolTipRole)
        elif not ports:
            combo.addItem("No COM")
        for index in range(combo.count()):
            if combo.itemText(index).split(" | ", 1)[0].upper() == selected_port:
                combo.setCurrentIndex(index)
                break
        combo.blockSignals(False)
        self._set_config_port("force_sensor" if combo is self.combo_force_port else "sma", combo.currentText())

    def _warn_selected_ports(self):
        ports = self._serial_ports or list_serial_ports()
        for label, cfg in [("力传感器", self.config["force_sensor"]), ("SMA", self.config["sma"])]:
            port_name = str(cfg.get("port", "")).upper()
            match = next((port for port in ports if port.get("device", "").upper() == port_name), None)
            if not match:
                self._log(f"警告：{label} 配置的端口 {port_name} 当前未检测到。")
                continue
            desc = str(match.get("description", ""))
            hwid = str(match.get("hwid", ""))
            if "蓝牙" in desc or "Bluetooth" in desc or "BTHENUM" in hwid:
                self._log(f"警告：{label} 端口 {port_name} 看起来像蓝牙虚拟串口，请确认是否为真实硬件。")

    def _start_local_devices(self):
        try:
            self.sensor.start()
            self.sensor_ok = True
            self._log("力传感器已启动。")
        except Exception as exc:
            self.sensor_ok = False
            self._log(f"力传感器启动失败: {exc}")
            self.diagnostics.exception("Force sensor start failed", exc)

        try:
            self.sma.start()
            self.sma_ok = True
            self._log("SMA 控制器已启动。")
        except Exception as exc:
            self.sma_ok = False
            self._log(f"SMA 控制器启动失败: {exc}")
            self.diagnostics.exception("SMA controller start failed", exc)

    def _reconnect_local_devices(self):
        self._log("正在重连力传感器和 SMA...")
        try:
            self.sensor.stop()
        except Exception as exc:
            self.diagnostics.exception("Force sensor stop before reconnect failed", exc)
        try:
            self.sma.close()
        except Exception as exc:
            self.diagnostics.exception("SMA close before reconnect failed", exc)
        self.sensor = create_force_sensor(self.config["force_sensor"])
        self.sma = SMAController(self.config["sma"])
        self.sensor_ok = False
        self.sma_ok = False
        self._start_local_devices()
        self._write_diagnostic_snapshot()

    def _list_com_ports(self):
        self._refresh_port_combos(log_result=False)
        ports = self._serial_ports
        self.diagnostics.snapshot("serial ports", ports)
        if not ports:
            self._log("未检测到串口。")
            return
        self._log("检测到以下串口:")
        for port in ports:
            if "error" in port:
                self._log(port["error"])
            else:
                self._log(f"{port.get('device')} | {port.get('description')} | {port.get('hwid')}")
        self._warn_selected_ports()

    def _test_force_sensor_once(self):
        try:
            if not self.sensor_ok:
                self.sensor.start()
                self.sensor_ok = True
            raw = self.sensor.read()
            state = {
                "raw_force": raw,
                "last_raw_frame": getattr(self.sensor, "last_raw_frame", ""),
                "last_parse_ok": getattr(self.sensor, "last_parse_ok", None),
                "last_parse_error": getattr(self.sensor, "last_parse_error", ""),
            }
            self.diagnostics.snapshot("force sensor test", state)
                self._log(f"力传感器测试结果: {state}")
        except Exception as exc:
            self.sensor_ok = False
            self._log(f"力传感器测试失败: {exc}")
            self.diagnostics.exception("Force sensor test failed", exc)

    def _test_sma_once(self):
        try:
            if not self.sma_ok:
                self.sma.start()
                self.sma_ok = True
            temp = self.sma.read_temperature()
            self.sma.stop()
            state = {
                "temperature": temp,
                "last_command": self.sma.last_command,
                "last_response": self.sma.last_response,
                "recent_responses": self.sma.recent_responses,
                "port": self.config["sma"].get("port"),
                "baud": self.config["sma"].get("baud"),
                "simulation": self.config["sma"].get("simulation"),
            }
            self.diagnostics.snapshot("sma test", state)
            self._log(f"SMA 测试结果: {state}")
        except Exception as exc:
            self.sma_ok = False
            self._log(f"SMA 测试失败: {exc}")
            self.diagnostics.exception("SMA test failed", exc)

    def _state_text(self, state_value: str) -> str:
        mapping = {
            "NONE": "未接触",
            "CONTACT": "已接触",
            "STABLE": "稳定夹持",
            "OVER": "过载",
            "none": "未接触",
            "contact": "已接触",
            "stable": "稳定夹持",
            "over": "过载",
        }
        return mapping.get(state_value, state_value)

    def _connect_cpp(self):
        if self.bridge:
            self.bridge.close()
            self.bridge = None
            self._set_connected(False)
            return
        cfg = self.config["cpp_bridge"]
        try:
            self.bridge = CppBridgeClient(cfg["host"], int(cfg["port"]), cfg["heartbeat_interval_ms"], cfg["timeout_ms"])
            self.bridge.connect()
            self._last_cpp_message_count = 0
            self._set_connected(True)
            self._log("已连接 C++ 硬件桥。")
        except Exception as exc:
            self.bridge = None
            self._set_connected(False)
            self._log(f"C++ 硬件桥连接失败: {exc}")
            self.diagnostics.exception("C++ bridge connection failed", exc)

    def _tick(self):
        now = time.monotonic()
        try:
            raw_force = self.sensor.read() if self.sensor_ok else {"fx": 0.0, "fy": 0.0, "fz": 0.0}
        except Exception as exc:
            raw_force = {"fx": 0.0, "fy": 0.0, "fz": 0.0}
            if now - self._last_sensor_error_log > 1.0:
                self._log(f"力传感器读取失败: {exc}")
                self.diagnostics.exception("Force sensor read failed", exc)
                self._last_sensor_error_log = now
        force = self.processor.update(raw_force)
        state, changed = self.state_machine.update(force["fn"], now)
        feedback = self.feedback_policy.compute(force["fn"], state)
        if self.sma_ok:
            self.sma.update_over_repeat(state)

        if changed and self.sma_ok:
            self.sma.on_grasp_state_changed(state)
            self._log(f"夹持状态变化: {self._state_text(state.value)}")

        telemetry = self.bridge.latest_telemetry() if self.bridge else {}
        if self.bridge:
            self._consume_cpp_messages()
            try:
                self.bridge.send_feedback(feedback)
            except Exception as exc:
                self._log(f"反馈力发送失败: {exc}")
                self.diagnostics.exception("Feedback send failed", exc)
                self.bridge.close()
                self.bridge = None
                self._set_connected(False)

        t = now - self.start_time
        self.t_points.append(t)
        self.fx_points.append(force["fx"])
        self.fy_points.append(force["fy"])
        self.fz_points.append(force["fz"])
        self.t_points = self.t_points[-500:]
        self.fx_points = self.fx_points[-500:]
        self.fy_points = self.fy_points[-500:]
        self.fz_points = self.fz_points[-500:]
        self.fx_curve.setData(self.t_points, self.fx_points)
        self.fy_curve.setData(self.t_points, self.fy_points)
        self.fz_curve.setData(self.t_points, self.fz_points)

        self.cards["fx"].set_value(force["fx"], 4)
        self.cards["fy"].set_value(force["fy"], 4)
        self.cards["fz"].set_value(force["fz"], 4)
        self.cards["fn"].set_value(force["fn"], 4)
        self.cards["state"].set_value(self._state_text(state.value))
        self.cards["feedback"].set_value(feedback, 3)
        self.cards["applied"].set_value(float(telemetry.get("gripper_feedback_applied", 0.0)), 3)
        self.cards["omega"].set_value(
            f"{telemetry.get('omega_px', 0.0):.3f}, {telemetry.get('omega_py', 0.0):.3f}, {telemetry.get('omega_pz', 0.0):.3f}"
        )
        self.cards["sensor"].set_value("正常" if self.sensor_ok else "未连接")
        self.cards["sma"].set_value("正常" if self.sma_ok else "未连接")
        self.cards["sma_temp"].set_value(self.sma.last_temperature, 1)
        self.cards["recording"].set_value("记录中" if self.recording else "未记录")

        if self.sma_ok and now - self._last_temperature_query >= float(self.config["sma"].get("temperature_query_interval_ms", 1000)) / 1000.0:
            try:
                self.sma.read_temperature()
            except Exception as exc:
                self._log(f"SMA 温度读取失败: {exc}")
                self.diagnostics.exception("SMA temperature read failed", exc)
            self._last_temperature_query = now

        if self.recording:
            self.logger.write_row({
                "timestamp": time.time(),
                "sample_index": self.sample_index,
                "fx": force["fx"],
                "fy": force["fy"],
                "fz": force["fz"],
                "fn": force["fn"],
                "ft": force["ft"],
                "grasp_state": state.value,
                "gripper_feedback_target": feedback,
                "gripper_feedback_applied": telemetry.get("gripper_feedback_applied", 0.0),
                "omega_px": telemetry.get("omega_px", 0.0),
                "omega_py": telemetry.get("omega_py", 0.0),
                "omega_pz": telemetry.get("omega_pz", 0.0),
                "cpp_warning": telemetry.get("cpp_warning", ""),
                "sma_command": self.sma.last_command,
                "sma_temperature": self.sma.last_temperature,
            })

        self.sample_index += 1

    def _toggle_recording(self):
        if self.recording:
            self.logger.stop()
            self.recording = False
            self.btn_record.setText("开始记录")
            self._log("数据记录已停止。")
            return
        path = self.logger.start()
        self.recording = True
        self.btn_record.setText("停止记录")
        self._log(f"数据记录已开始: {path}")

    def _write_diagnostic_snapshot(self):
        telemetry = self.bridge.latest_telemetry() if self.bridge else {}
        snapshot = {
            "time": time.strftime("%Y-%m-%d %H:%M:%S"),
            "config_path": self.config_path,
            "cpp_connected": self.bridge is not None,
            "force_sensor_ok": self.sensor_ok,
            "force_sensor_selected_port": self.config["force_sensor"].get("port", ""),
            "sma_ok": self.sma_ok,
            "sma_selected_port": self.config["sma"].get("port", ""),
            "recording": self.recording,
            "sample_index": self.sample_index,
            "last_sma_command": self.sma.last_command,
            "last_sma_response": self.sma.last_response,
            "recent_sma_responses": self.sma.recent_responses,
            "last_sma_temperature": self.sma.last_temperature,
            "force_sensor_last_raw_frame": getattr(self.sensor, "last_raw_frame", ""),
            "force_sensor_last_parse_ok": getattr(self.sensor, "last_parse_ok", None),
            "force_sensor_last_parse_error": getattr(self.sensor, "last_parse_error", ""),
            "serial_ports": list_serial_ports(),
            "latest_cpp_telemetry": telemetry,
            "recent_cpp_messages": self.bridge.recent_messages() if self.bridge else [],
            "diagnostics_file": str(self.diagnostics.path),
            "experiment_file": str(self.logger.current_path) if self.logger.current_path else "",
        }
        self.diagnostics.snapshot("manual snapshot", snapshot)
        QGuiApplication.clipboard().setText(str(self.diagnostics.path))
        self._log(f"诊断信息已保存，路径已复制: {self.diagnostics.path}")

    def _consume_cpp_messages(self):
        messages = self.bridge.recent_messages() if self.bridge else []
        if len(messages) <= self._last_cpp_message_count:
            return
        for msg in messages[self._last_cpp_message_count:]:
            msg_type = msg.get("type")
            if msg_type == "error":
                self._log(f"C++ 返回错误: {msg}")
                self.diagnostics.snapshot("cpp error", msg)
            elif msg_type == "ack":
                self.diagnostics.snapshot("cpp ack", msg)
        self._last_cpp_message_count = len(messages)

    def _send_command(self, command: str):
        if not self.bridge:
            self._log("C++ 硬件桥尚未连接。")
            return
        try:
            self.bridge.send_command(command)
            self._log(f"已发送命令: {command}")
        except Exception as exc:
            self._log(f"命令发送失败: {exc}")
            self.diagnostics.exception(f"Command failed: {command}", exc)

    def _emergency_stop(self):
        if self.bridge:
            try:
                self.bridge.send_feedback(0.0)
                self.bridge.send_command("emergency_stop")
            except Exception as exc:
                self._log(f"C++ 急停命令失败: {exc}")
                self.diagnostics.exception("C++ emergency stop failed", exc)
        if self.sma_ok:
            try:
                self.sma.emergency_stop()
            except Exception as exc:
                self._log(f"SMA 急停命令失败: {exc}")
                self.diagnostics.exception("SMA emergency stop failed", exc)
        self._log("已请求急停。")

    def _set_connected(self, connected: bool):
        self.status.setText("C++ 已连接" if connected else "C++ 未连接")
        self.status.setObjectName("statusConnected" if connected else "statusDisconnected")
        self.status.style().unpolish(self.status)
        self.status.style().polish(self.status)
        self.btn_connect.setText("断开 C++" if connected else "连接 C++")

    def _log(self, text: str):
        self.log.appendPlainText(time.strftime("%H:%M:%S ") + text)
        self.diagnostics.log(text)

    def closeEvent(self, event):
        try:
            self._write_diagnostic_snapshot()
            if self.bridge:
                self.bridge.send_feedback(0.0)
                self.bridge.send_command("stop_experiment")
                self.bridge.close()
            self.sma.close()
            self.sensor.stop()
            self.logger.stop()
            self.diagnostics.close()
        finally:
            super().closeEvent(event)


def run_ui(config_path: str):
    app = QApplication([])
    apply_application_font()
    app.setStyle("Fusion")
    window = MainWindow(config_path)

    def handle_unhandled_exception(exc_type, exc_value, exc_traceback):
        text = "".join(traceback.format_exception(exc_type, exc_value, exc_traceback))
        window.diagnostics.write_section("unhandled exception", text)
        window._log(f"程序未处理异常: {exc_value}")

    sys.excepthook = handle_unhandled_exception
    window.show()
    app.exec()
