import os
from datetime import datetime
import time
from PySide6.QtWidgets import (QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
                               QLabel, QLineEdit, QPushButton, QCheckBox,
                               QGroupBox, QGridLayout, QFormLayout, QFileDialog, QStatusBar, QMessageBox,
                               QSpacerItem, QSizePolicy, QSplitter, QScrollArea, QFrame, QListView, QButtonGroup,
                               QStackedWidget)
from PySide6.QtCore import Qt, QTimer, QSize, Signal, QObject
from PySide6.QtGui import QFontMetrics, QIcon, QPixmap
from PySide6.QtSvgWidgets import QSvgWidget
import ctypes
import pyqtgraph as pg

from .advanced_settings_dialog import AdvancedSettingsDialog
from .widgets.model_viewer import ModelViewer
from ..workers.acquisition_worker import AcquisitionWorker
from ..data.data_buffer import DataBuffer
from ..data.async_data_recorder import AsyncDataRecorder

class SegmentedControl(QWidget):
    currentChanged = Signal(str)

    def __init__(self, options, current=None):
        super().__init__()
        self.setObjectName("segmentedControl")
        layout = QHBoxLayout(self)
        layout.setContentsMargins(4, 4, 4, 4)
        layout.setSpacing(2)

        self.group = QButtonGroup(self)
        self.group.setExclusive(True)
        self.group.buttonClicked.connect(self._on_button_clicked)

        self._buttons = {}
        for idx, option in enumerate(options):
            btn = QPushButton(option)
            btn.setCheckable(True)
            btn.setProperty("class", "segmentButton")
            self.group.addButton(btn, idx)
            layout.addWidget(btn)
            self._buttons[option] = btn

            if current and option == current:
                btn.setChecked(True)

        if not current and options:
            self._buttons[options[0]].setChecked(True)

    def currentText(self):
        btn = self.group.checkedButton()
        if btn:
            return btn.text()
        return ""

    def setCurrentText(self, text):
        if text in self._buttons:
            self._buttons[text].setChecked(True)

    def _on_button_clicked(self, button):
        self.currentChanged.emit(button.text())


class ValueCard(QWidget):
    def __init__(self, title, unit, color, compact=False):
        super().__init__()
        self.compact = compact
        self.setObjectName("valCard")

        layout = QGridLayout(self)
        margins = 6 if compact else 12
        layout.setContentsMargins(margins, margins, margins, margins)
        layout.setHorizontalSpacing(4)
        layout.setVerticalSpacing(2 if compact else 6)

        color_indicator = QLabel()
        ind_size = 8 if compact else 10
        color_indicator.setFixedSize(ind_size, ind_size)
        color_indicator.setStyleSheet(f"background-color: {color}; border-radius: {ind_size//2}px;")

        title_label = QLabel(title)
        title_label.setProperty("class", "channel-title-compact" if compact else "channel-title")

        layout.addWidget(color_indicator, 0, 0, Qt.AlignLeft | Qt.AlignVCenter)
        layout.addWidget(title_label, 0, 1, 1, 2, Qt.AlignLeft | Qt.AlignVCenter)

        self.val_label = QLabel("0.0000")
        self.val_label.setProperty("class", "channel-value-compact" if compact else "channel-value")
        self.val_label.setAlignment(Qt.AlignLeft | Qt.AlignVCenter)
        self.val_label.setMinimumWidth(80)

        unit_label = QLabel(unit)
        unit_label.setProperty("class", "channel-unit-compact" if compact else "channel-unit")
        unit_label.setAlignment(Qt.AlignLeft | Qt.AlignBottom)

        layout.addWidget(self.val_label, 1, 0, 1, 2, Qt.AlignLeft | Qt.AlignVCenter)
        layout.addWidget(unit_label, 1, 2, Qt.AlignLeft | Qt.AlignBottom)

        layout.setColumnStretch(1, 1)

    def set_value(self, val):
        self.val_label.setText(f"{val:.4f}")

class AlarmCard(QWidget):
    def __init__(self, title):
        super().__init__()
        self.setObjectName("alarmCard")
        layout = QVBoxLayout(self)
        layout.setContentsMargins(10, 10, 10, 10)
        layout.setSpacing(4)

        header_layout = QHBoxLayout()
        header_layout.setContentsMargins(0, 0, 0, 0)

        self.title_label = QLabel(title)
        self.title_label.setProperty("class", "alarm-title")

        self.status_label = QLabel("未配置")
        self.status_label.setProperty("class", "alarm-status-unconfigured")
        self.status_label.setAlignment(Qt.AlignRight | Qt.AlignVCenter)

        header_layout.addWidget(self.title_label)
        header_layout.addWidget(self.status_label)

        self.desc_label = QLabel("等待规则配置")
        self.desc_label.setProperty("class", "alarm-desc")
        self.desc_label.setWordWrap(True)

        layout.addLayout(header_layout)
        layout.addWidget(self.desc_label)

    def set_state(self, title, level, reason):
        self.title_label.setText(title)
        self.status_label.setText(level)
        self.desc_label.setText(reason)

        if level == "未触发":
            self.status_label.setProperty("class", "alarm-status-normal")
            self.status_label.setStyleSheet("color: #94A3B8;")
        elif level == "预警":
            self.status_label.setProperty("class", "alarm-status-warning")
            self.status_label.setStyleSheet("color: #F59E0B;")
        elif level == "已触发":
            self.status_label.setProperty("class", "alarm-status-danger")
            self.status_label.setStyleSheet("color: #EF4444;")
        else:
            self.status_label.setProperty("class", "alarm-status-unconfigured")
            self.status_label.setStyleSheet("color: #64748B;")

        self.style().unpolish(self.status_label)
        self.style().polish(self.status_label)


class ImagePreviewWidget(QLabel):
    def __init__(self, image_path, parent=None):
        super().__init__(parent)
        self.image_path = image_path
        self._pixmap = QPixmap(image_path) if image_path and os.path.exists(image_path) else QPixmap()
        self.setAlignment(Qt.AlignCenter)
        self.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self.setMinimumSize(120, 120)
        if self._pixmap.isNull():
            self.setText("未找到模型预览图片\nassets/images/model_preview.png")
            self.setStyleSheet("color: #94A3B8; border: 1px dashed #475569; border-radius: 4px;")
        else:
            self.setStyleSheet("border: none;")
            self._update_scaled_pixmap()

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._update_scaled_pixmap()

    def _update_scaled_pixmap(self):
        if self._pixmap.isNull():
            return
        size = self.contentsRect().size()
        if size.width() <= 0 or size.height() <= 0:
            return
        self.setPixmap(self._pixmap.scaled(size, Qt.KeepAspectRatio, Qt.SmoothTransformation))


class ModelLoadBridge(QObject):
    model_loaded = Signal(object)


class MainWindow(QMainWindow):
    def __init__(self, config, logger):
        super().__init__()
        self.config = config
        self.logger = logger
        self.current_theme = self.config.get("ui", {}).get("theme", "light")
        self.setWindowTitle("FaWave 多维力感知平台")
        from ..utils.resource import resource_path

        # Prefer the generated ICO file, fallback to SVG if missing
        icon_path = resource_path("assets/app_icon.ico")
        if not os.path.exists(icon_path):
            icon_path = resource_path("assets/app_icon.svg")

        if os.path.exists(icon_path):
            self.setWindowIcon(QIcon(icon_path))

        # Dynamically scale sizes to match target monitor width to prevent clipping
        screen = QApplication.primaryScreen().availableGeometry()
        w = min(1500, int(screen.width() * 0.9))
        h = min(900, int(screen.height() * 0.9))

        self.setMinimumSize(1280, 720)
        self.resize(w, h)

        self.last_error = "无"

        self.data_buffer = DataBuffer(max_points=config.get("ui", {}).get("max_plot_points", 2000))
        self.data_recorder = AsyncDataRecorder(config)
        self.worker = AcquisitionWorker(config, self.data_buffer, self.data_recorder, self.logger)

        self.worker.connection_status_changed.connect(self.on_connection_status_changed)
        self.worker.error_occurred.connect(self.on_error_occurred)

        self.ui_timer = QTimer(self)
        self.refresh_rate_ms = config.get("ui", {}).get("refresh_rate_ms", 50)
        self.ui_timer.timeout.connect(self.update_ui)

        self.setup_ui()
        self.apply_theme()
        if hasattr(self, 'task_mode_combo'):
            self.update_alarm_ui(self.task_mode_combo.currentText())

    def setup_ui(self):
        main_widget = QWidget()
        main_widget.setObjectName("centralWidget")
        self.setCentralWidget(main_widget)
        main_layout = QVBoxLayout(main_widget)
        main_layout.setContentsMargins(16, 16, 16, 8)
        main_layout.setSpacing(16)

        self.setup_header(main_layout)

        # Main Content Area - Use QSplitter for horizontal resizing robustly
        h_splitter = QSplitter(Qt.Horizontal)
        h_splitter.setObjectName("mainSplitter")
        h_splitter.setHandleWidth(4)
        main_layout.addWidget(h_splitter, stretch=1)

        self.setup_left_panel(h_splitter)
        self.setup_center_panel(h_splitter)
        self.setup_right_panel(h_splitter)

        # Match the initial three-column balance from the target layout.
        h_splitter.setSizes([360, 980, 340])

        self.setup_status_bar()

    def setup_header(self, parent_layout):
        header_frame = QFrame()
        header_frame.setObjectName("headerCard")
        header_layout = QHBoxLayout(header_frame)
        header_layout.setContentsMargins(20, 16, 20, 16)

        logo_layout = QHBoxLayout()
        from ..utils.resource import resource_path
        logo_path = resource_path("assets/logo.svg")
        if os.path.exists(logo_path):
            self.logo_widget = QSvgWidget(logo_path)
            self.logo_widget.setFixedSize(40, 40)
            logo_layout.addWidget(self.logo_widget)
            logo_layout.addSpacing(12)

        title_layout = QVBoxLayout()
        self.title_label = QLabel("FaWave 多维力感知采集与安全监测平台")
        self.title_label.setObjectName("headerTitle")

        self.subtitle_label = QLabel("面向多通道力传感、三维力解耦与安全报警的实时采集系统")
        self.subtitle_label.setObjectName("headerSubtitle")

        title_layout.addWidget(self.title_label)
        title_layout.addWidget(self.subtitle_label)
        logo_layout.addLayout(title_layout)

        # Header Right side
        header_right_layout = QHBoxLayout()

        # Theme toggle Card-like
        theme_widget = QFrame()
        theme_widget.setObjectName("valCard")
        theme_layout = QHBoxLayout(theme_widget)
        theme_layout.setContentsMargins(12, 6, 12, 6)
        theme_label = QLabel("界面主题")
        theme_label.setProperty("class", "sys-stat-label")
        self.btn_theme = QPushButton("切换深色主题" if self.current_theme == 'light' else "切换浅色主题")
        self.btn_theme.setCursor(Qt.PointingHandCursor)
        self.btn_theme.clicked.connect(self.toggle_theme)
        theme_layout.addWidget(theme_label)
        theme_layout.addWidget(self.btn_theme)

        # Task Mode Switch
        task_mode_widget = QFrame()
        task_mode_widget.setObjectName("valCard")
        task_mode_layout = QHBoxLayout(task_mode_widget)
        task_mode_layout.setContentsMargins(12, 6, 12, 6)
        task_label = QLabel("任务模式")
        task_label.setProperty("class", "sys-stat-label")
        self.task_mode_combo = SegmentedControl(["牵拉模式", "剪切模式"], "牵拉模式")
        self.task_mode_combo.currentChanged.connect(self.on_task_mode_changed)
        task_mode_layout.addWidget(task_label)
        task_mode_layout.addWidget(self.task_mode_combo)

        self.btn_advanced = QPushButton("⚙ 高级设置")
        self.btn_advanced.setObjectName("btnAdvanced")
        self.btn_advanced.clicked.connect(self.open_advanced_settings)

        self.status_capsule = QLabel("● 未连接")
        self.status_capsule.setObjectName("statusCapsule_Disconnected")
        self.status_capsule.setAlignment(Qt.AlignCenter)

        header_right_layout.addWidget(task_mode_widget)
        header_right_layout.addSpacing(16)
        header_right_layout.addWidget(theme_widget)
        header_right_layout.addSpacing(16)
        header_right_layout.addWidget(self.btn_advanced)
        header_right_layout.addSpacing(16)
        header_right_layout.addWidget(self.status_capsule)

        header_layout.addLayout(logo_layout)
        header_layout.addStretch()
        header_layout.addLayout(header_right_layout)
        parent_layout.addWidget(header_frame)

    def setup_left_panel(self, parent_layout):
        left_scroll = QScrollArea()
        left_scroll.setMinimumWidth(290)
        left_scroll.setMaximumWidth(330)
        left_scroll.setWidgetResizable(True)
        left_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)

        left_panel = QWidget()
        left_layout = QVBoxLayout(left_panel)
        left_layout.setContentsMargins(0, 0, 10, 0)
        left_layout.setSpacing(16)

        # 1. Communication
        conn_group = QGroupBox("通信设置")
        conn_layout = QFormLayout(conn_group)
        conn_layout.setSpacing(12)

        self.ip_input = QLineEdit(self.config.get("device_ip", "192.168.1.82"))
        self.port_input = QLineEdit(str(self.config.get("device_port", 16008)))

        # Arrange settings vertically per user request
        conn_layout.addRow(QLabel("设备 IP"))
        conn_layout.addRow(self.ip_input)

        conn_layout.addRow(QLabel("端口"))
        conn_layout.addRow(self.port_input)

        conn_layout.addRow(QLabel("采集模式"))

        default_acq_mode = "真实设备"
        if self.config.get("acquisition_mode") == "simulation":
            default_acq_mode = "仿真演示"

        self.mode_combo = SegmentedControl(["真实设备", "仿真演示"], default_acq_mode)
        conn_layout.addRow(self.mode_combo)

        conn_layout.addRow(QLabel("请求间隔 / ms"))
        self.interval_input = QLineEdit(str(self.config.get("request_interval_ms", 20)))
        conn_layout.addRow(self.interval_input)

        self.btn_connect = QPushButton("建立连接")
        self.btn_connect.setObjectName("btnConnect")
        self.btn_connect.setMinimumHeight(42)
        self.btn_connect.clicked.connect(self.toggle_connection)
        conn_layout.addRow(self.btn_connect)
        left_layout.addWidget(conn_group)

        # 2. Data Recording
        record_group = QGroupBox("数据记录")
        record_layout = QVBoxLayout(record_group)
        record_layout.setSpacing(12)

        self.record_checkbox = QCheckBox("启用本地存储")
        self.record_checkbox.stateChanged.connect(self.on_record_toggled)

        fmt_layout = QVBoxLayout()
        fmt_layout.setSpacing(6)
        fmt_layout.addWidget(QLabel("保存格式"))
        self.format_combo = SegmentedControl(["CSV", "XLSX"], "CSV")
        fmt_layout.addWidget(self.format_combo)

        self.path_btn = QPushButton("选择保存路径")
        self.path_btn.setMinimumHeight(40)
        self.path_btn.clicked.connect(self.select_save_path)

        self.save_path = ""
        self.path_label = QLabel(self.save_path)
        self.path_label.setProperty("class", "sys-stat-label")
        self.path_label.setToolTip(self.save_path)

        # Elide text if too long
        metrics = QFontMetrics(self.path_label.font())
        elided = metrics.elidedText(self.save_path, Qt.ElideMiddle, 300)
        self.path_label.setText(elided)

        record_layout.addWidget(self.record_checkbox)
        record_layout.addLayout(fmt_layout)
        record_layout.addWidget(self.path_btn)
        record_layout.addWidget(QLabel("当前路径:"))
        record_layout.addWidget(self.path_label)
        left_layout.addWidget(record_group)

        # 3. Controls
        ctrl_group = QGroupBox("操作控制")
        ctrl_layout = QVBoxLayout(ctrl_group)
        ctrl_layout.setSpacing(12)

        self.btn_autoscale = QPushButton("自动跟随：开启")
        self.btn_autoscale.setMinimumHeight(40)
        self.btn_autoscale.clicked.connect(self.toggle_auto_follow)

        self.auto_follow = True
        self.follow_window_s = 10.0
        self._programmatic_range_update = False

        self.btn_clear = QPushButton("清空波形")
        self.btn_clear.setMinimumHeight(40)
        self.btn_clear.clicked.connect(self.clear_plot)

        self.btn_zero_force = QPushButton("三维力归零")
        self.btn_zero_force.setMinimumHeight(40)
        self.btn_zero_force.clicked.connect(self.zero_force)

        ctrl_layout.addWidget(self.btn_autoscale)
        ctrl_layout.addWidget(self.btn_clear)
        ctrl_layout.addWidget(self.btn_zero_force)
        left_layout.addWidget(ctrl_group)

        left_layout.addStretch()
        left_scroll.setWidget(left_panel)
        parent_layout.addWidget(left_scroll)

    def setup_center_panel(self, parent_layout):
        center_panel = QWidget()
        center_layout = QVBoxLayout(center_panel)
        center_layout.setContentsMargins(0, 0, 0, 0)
        center_layout.setSpacing(0)

        center_vertical_splitter = QSplitter(Qt.Vertical)
        center_vertical_splitter.setObjectName("centerVerticalSplitter")
        center_vertical_splitter.setHandleWidth(4)
        center_vertical_splitter.setChildrenCollapsible(False)

        top_row_splitter = QSplitter(Qt.Horizontal)
        top_row_splitter.setObjectName("topRowSplitter")
        top_row_splitter.setHandleWidth(4)
        top_row_splitter.setChildrenCollapsible(False)

        plot_splitter = QSplitter(Qt.Vertical)
        plot_splitter.setObjectName("plotSplitter")
        plot_splitter.setHandleWidth(4)
        plot_splitter.setChildrenCollapsible(True)

        model_card = QWidget()
        model_card.setProperty("class", "Card")
        model_card.setMinimumSize(220, 180)
        model_card.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        model_layout = QVBoxLayout(model_card)
        model_layout.setContentsMargins(8, 8, 8, 8)
        model_layout.setSpacing(8)

        title_row = QHBoxLayout()
        model_title = QLabel("传感器模型展示")
        model_title.setStyleSheet("font-size: 15px; font-weight: bold;")
        title_row.addWidget(model_title)

        self.btn_toggle_3d = QPushButton("查看 3D 模型")
        self.btn_toggle_3d.setStyleSheet("padding: 4px 8px; font-size: 12px; border-radius: 4px;")
        self.btn_toggle_3d.clicked.connect(self.toggle_3d_view)
        title_row.addWidget(self.btn_toggle_3d)
        title_row.addStretch()
        model_layout.addLayout(title_row)

        self.model_stacked_widget = QStackedWidget()
        self.model_stacked_widget.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)

        # Page 0: Image Preview
        from ..utils.resource import resource_path
        img_path = resource_path("assets/images/model_preview.png")
        self.preview_label = ImagePreviewWidget(img_path)

        self.model_stacked_widget.addWidget(self.preview_label)

        self.gl_viewport = None
        self.model_load_result = None
        self.model_loading = False
        self.model_rendered = False
        self.model_pending_show = False
        self.model_preload_started = False
        model_layout.addWidget(self.model_stacked_widget, stretch=1)

        overview_card = self.create_value_cards()
        overview_card.setMinimumSize(480, 180)
        top_row_splitter.addWidget(model_card)
        top_row_splitter.addWidget(overview_card)
        top_row_splitter.setSizes([280, 700])

        self.setup_voltage_plot(plot_splitter)
        self.setup_force_plot(plot_splitter)
        plot_splitter.setCollapsible(0, True)
        plot_splitter.setCollapsible(1, True)
        plot_splitter.setSizes([230, 205])

        center_vertical_splitter.addWidget(top_row_splitter)
        center_vertical_splitter.addWidget(plot_splitter)
        center_vertical_splitter.setSizes([300, 430])
        center_layout.addWidget(center_vertical_splitter, stretch=1)
        self._prepare_3d_model_before_show()

        # Safe addition handling QSplitter or QBoxLayout
        if isinstance(parent_layout, QSplitter):
            parent_layout.addWidget(center_panel)
        else:
            parent_layout.addWidget(center_panel, stretch=1)

    def create_value_cards(self):
        overview_card = QWidget()
        overview_card.setProperty("class", "Card")
        overview_card.setMinimumHeight(180)
        overview_card.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)

        layout = QVBoxLayout(overview_card)
        layout.setContentsMargins(12, 12, 12, 12)
        layout.setSpacing(10)

        title = QLabel("实时数据总览")
        title.setStyleSheet("font-size: 15px; font-weight: bold;")
        layout.addWidget(title)

        grid = QGridLayout()
        grid.setSpacing(10)

        # Row 1 Headers
        grid.addWidget(QLabel("<b>原始电压</b>"), 0, 0, 1, 4)

        # Voltages
        self.card_ch1 = ValueCard("通道 1", "V", "#2563EB", compact=True)
        self.card_ch2 = ValueCard("通道 2", "V", "#F97316", compact=True)
        self.card_ch3 = ValueCard("通道 3", "V", "#10B981", compact=True)
        self.card_ch4 = ValueCard("通道 4", "V", "#8B5CF6", compact=True)

        grid.addWidget(self.card_ch1, 1, 0)
        grid.addWidget(self.card_ch2, 1, 1)
        grid.addWidget(self.card_ch3, 1, 2)
        grid.addWidget(self.card_ch4, 1, 3)

        # Row 2 Headers
        f_header = QHBoxLayout()
        f_header.setContentsMargins(0, 0, 0, 0)
        f_title = QLabel("<b>三维力</b>")
        self.f_status = QLabel("解耦未启用")
        self.f_status.setStyleSheet("color: #94A3B8; font-size: 12px;")
        f_header.addWidget(f_title)
        f_header.addWidget(self.f_status)
        f_header.addStretch()
        f_header_widget = QWidget()
        f_header_widget.setLayout(f_header)
        grid.addWidget(f_header_widget, 2, 0, 1, 4)

        # Forces
        self.card_fx = ValueCard("Fx", "N", "#0EA5E9", compact=True)
        self.card_fy = ValueCard("Fy", "N", "#F59E0B", compact=True)
        self.card_fz = ValueCard("Fz", "N", "#EF4444", compact=True)

        grid.addWidget(self.card_fx, 3, 0)
        grid.addWidget(self.card_fy, 3, 1)
        grid.addWidget(self.card_fz, 3, 2)

        # Blank placeholder for bottom right corner to maintain equal layout widths
        spacer_card = QWidget()
        grid.addWidget(spacer_card, 3, 3)

        for i in range(4):
            grid.setColumnStretch(i, 1)

        layout.addLayout(grid)

        return overview_card

    def setup_value_cards(self, parent_layout):
        parent_layout.addWidget(self.create_value_cards(), stretch=0)

    def create_legend_toggle_chip(self, text, color):
        btn = QPushButton(f"● {text}")
        btn.setProperty("class", "legend-chip")
        btn.setCheckable(True)
        btn.setChecked(True)
        # We handle dynamic color switching in style logic or directly
        btn.setStyleSheet(f"""
            QPushButton:checked {{
                color: {color};
            }}
        """)
        return btn

    def setup_voltage_plot(self, parent_splitter):
        container = QWidget()
        container.setProperty("class", "Card")
        container.setMinimumHeight(0)
        container.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        layout = QVBoxLayout(container)

        # Header Row
        header_layout = QHBoxLayout()
        title = QLabel("原始电压曲线")
        title.setStyleSheet("font-size: 15px; font-weight: bold;")
        header_layout.addWidget(title)
        header_layout.addStretch()

        # Custom Legend Toggle Chips
        self.chk_ch1 = self.create_legend_toggle_chip("通道 1", "#2563EB")
        self.chk_ch2 = self.create_legend_toggle_chip("通道 2", "#F97316")
        self.chk_ch3 = self.create_legend_toggle_chip("通道 3", "#10B981")
        self.chk_ch4 = self.create_legend_toggle_chip("通道 4", "#8B5CF6")

        for chk in [self.chk_ch1, self.chk_ch2, self.chk_ch3, self.chk_ch4]:
            chk.toggled.connect(self.update_plot_visibility)
            header_layout.addWidget(chk)

        layout.addLayout(header_layout)

        pg.setConfigOption('background', 'w') # Will be overridden in apply_theme
        pg.setConfigOption('foreground', 'k')

        self.plot_voltage = pg.PlotWidget()
        self.plot_voltage.showGrid(x=True, y=True, alpha=0.3)
        self.plot_voltage.setLabel('left', '电压 (V)')
        self.plot_voltage.setLabel('bottom', '相对时间', units='s')
        self.plot_voltage.setYRange(-2.5, 2.5)

        self.curve_ch1 = self.plot_voltage.plot(pen=pg.mkPen('#2563EB', width=2))
        self.curve_ch2 = self.plot_voltage.plot(pen=pg.mkPen('#F97316', width=2))
        self.curve_ch3 = self.plot_voltage.plot(pen=pg.mkPen('#10B981', width=2))
        self.curve_ch4 = self.plot_voltage.plot(pen=pg.mkPen('#8B5CF6', width=2))

        self.plot_voltage.getViewBox().sigRangeChanged.connect(self.on_plot_interacted)

        layout.addWidget(self.plot_voltage)
        parent_splitter.addWidget(container)

    def setup_force_plot(self, parent_splitter):
        container = QWidget()
        container.setProperty("class", "Card")
        container.setMinimumHeight(0)
        container.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        layout = QVBoxLayout(container)

        # Header Row
        header_layout = QHBoxLayout()
        title = QLabel("三维力解耦曲线")
        title.setStyleSheet("font-size: 15px; font-weight: bold;")
        header_layout.addWidget(title)
        header_layout.addStretch()

        # Custom Legend Toggle Chips
        self.chk_fx = self.create_legend_toggle_chip("Fx", "#0EA5E9")
        self.chk_fy = self.create_legend_toggle_chip("Fy", "#F59E0B")
        self.chk_fz = self.create_legend_toggle_chip("Fz", "#EF4444")

        for chk in [self.chk_fx, self.chk_fy, self.chk_fz]:
            chk.toggled.connect(self.update_plot_visibility)
            header_layout.addWidget(chk)

        layout.addLayout(header_layout)

        self.plot_force = pg.PlotWidget()
        self.plot_force.showGrid(x=True, y=True, alpha=0.3)
        self.plot_force.setLabel('left', '力', units='N')
        self.plot_force.setLabel('bottom', '相对时间', units='s')
        self.plot_force.setYRange(-5, 5)

        self.curve_fx = self.plot_force.plot(pen=pg.mkPen('#0EA5E9', width=2))
        self.curve_fy = self.plot_force.plot(pen=pg.mkPen('#F59E0B', width=2))
        self.curve_fz = self.plot_force.plot(pen=pg.mkPen('#EF4444', width=2))

        self.plot_force.getViewBox().sigRangeChanged.connect(self.on_plot_interacted)

        layout.addWidget(self.plot_force)
        parent_splitter.addWidget(container)

    def setup_right_panel(self, parent_layout):
        right_scroll = QScrollArea()
        right_scroll.setMinimumWidth(280)
        right_scroll.setMaximumWidth(340)
        right_scroll.setWidgetResizable(True)
        right_scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)

        right_panel = QWidget()
        right_layout = QVBoxLayout(right_panel)
        right_layout.setContentsMargins(10, 0, 0, 0)
        right_layout.setSpacing(16)

        self.setup_alarm_panel(right_layout)
        self.setup_system_status_panel(right_layout)

        right_layout.addStretch()
        right_scroll.setWidget(right_panel)
        parent_layout.addWidget(right_scroll)

    def setup_alarm_panel(self, parent_layout):
        self.alarm_card_widget = QWidget()
        self.alarm_card_widget.setProperty("class", "Card")
        self.alarm_layout = QVBoxLayout(self.alarm_card_widget)
        self.alarm_layout.setSpacing(8)

        title = QLabel("安全报警")
        title.setStyleSheet("font-size: 15px; font-weight: bold;")
        self.alarm_layout.addWidget(title)

        self.alarm_cards = []
        # Pre-allocate 4 slots (max for suturing mode)
        for i in range(4):
            card = AlarmCard("")
            card.hide()
            self.alarm_cards.append(card)
            self.alarm_layout.addWidget(card)

        # Recent Alarms
        self.alarm_layout.addSpacing(8)
        recent_title = QLabel("最近报警")
        recent_title.setStyleSheet("font-weight: bold;")
        self.alarm_layout.addWidget(recent_title)

        self.recent_alarm_label = QLabel("暂无报警信息")
        self.recent_alarm_label.setProperty("class", "sys-stat-label")
        self.recent_alarm_label.setWordWrap(True)
        self.recent_alarm_label.setStyleSheet("background-color: transparent; border: 1px solid #DDE5F0; border-radius: 4px; padding: 6px;")
        self.alarm_layout.addWidget(self.recent_alarm_label)

        parent_layout.addWidget(self.alarm_card_widget)

    def update_alarm_ui(self, mode):
        # Refresh the UI layout properties for alarms dynamically
        try:
             from ..safety.safety_monitor import SafetyMonitor
             if not hasattr(self, 'safety_monitor'):
                  self.safety_monitor = SafetyMonitor(self.config)

             self.safety_monitor.set_task_mode(mode)
             current = self.safety_monitor.get_current_alarms()
             for i, card in enumerate(self.alarm_cards):
                  if i < len(current):
                       cfg = current[i]
                       card.set_state(f"● {cfg['event']}", cfg['level'], cfg['reason'])
                       card.show()
                  else:
                       card.hide()
        except ImportError:
             pass

    def setup_system_status_panel(self, parent_layout):
        sys_card = QWidget()
        sys_card.setProperty("class", "Card")
        layout = QVBoxLayout(sys_card)
        layout.setSpacing(12)

        title = QLabel("系统状态")
        title.setStyleSheet("font-size: 15px; font-weight: bold;")
        layout.addWidget(title)

        grid = QGridLayout()
        grid.setVerticalSpacing(12)
        grid.setHorizontalSpacing(16)

        labels = ["当前任务", "连接状态", "采集模式", "有效帧", "错误帧", "运行时间", "保存状态", "解耦状态", "算法状态", "输入单位"]
        self.sys_values = {}

        for i, lbl in enumerate(labels):
            l = QLabel(f"{lbl}")
            l.setProperty("class", "sys-stat-label")
            grid.addWidget(l, i, 0)

            v = QLabel("--")
            v.setProperty("class", "sys-stat-value")
            grid.addWidget(v, i, 1, alignment=Qt.AlignRight)
            self.sys_values[lbl] = v

        # Add Recent Error specifically
        layout.addLayout(grid)
        layout.addSpacing(8)

        err_title = QLabel("最近错误")
        err_title.setProperty("class", "sys-stat-label")
        layout.addWidget(err_title)

        self.sys_values["最近错误"] = QLabel("无")
        self.sys_values["最近错误"].setProperty("class", "sys-stat-value")
        self.sys_values["最近错误"].setWordWrap(True)
        layout.addWidget(self.sys_values["最近错误"])

        self.sys_values["当前任务"].setText("牵拉模式")
        self.sys_values["连接状态"].setText("未连接")
        self.sys_values["保存状态"].setText("未保存")
        self.sys_values["解耦状态"].setText("未启用")
        self.sys_values["算法状态"].setText("Python解耦")
        self.sys_values["输入单位"].setText(self.config.get("force_decoder", {}).get("input_unit", "V"))
        self.sys_values["采集模式"].setText(self.get_display_mode())

        parent_layout.addWidget(sys_card)

    def setup_status_bar(self):
        self.statusBar = QStatusBar()
        self.setStatusBar(self.statusBar)
        self.update_status()

    def apply_theme(self):
        try:
            from ..utils.resource import resource_path
            theme_file = 'light.qss' if self.current_theme == 'light' else 'dark.qss'
            qss_path = resource_path(f"src/ui/themes/{theme_file}")
            with open(qss_path, 'r', encoding='utf-8') as f:
                stylesheet = f.read()

            # Apply the stylesheet globally to all top-level widgets so Dialogs catch it automatically
            app = QApplication.instance()
            if app:
                app.setStyleSheet(stylesheet)

                # Force polish on all widgets to prevent artifacting
                for widget in app.allWidgets():
                    widget.style().unpolish(widget)
                    widget.style().polish(widget)
                    widget.update()
            else:
                self.setStyleSheet(stylesheet)

            # Update pg plots background
            bg_color = '#FFFFFF' if self.current_theme == 'light' else '#0B1120'
            fg_color = '#0F172A' if self.current_theme == 'light' else '#E5E7EB'
            grid_alpha = 50 if self.current_theme == 'light' else 80

            for plot in [self.plot_voltage, self.plot_force]:
                plot.setBackground(bg_color)
                plot.getPlotItem().getViewBox().setBackgroundColor(bg_color)
                plot.setStyleSheet("background: transparent; border: none;")
                plot.getAxis('left').setPen(fg_color)
                plot.getAxis('bottom').setPen(fg_color)
                plot.getAxis('left').setTextPen(fg_color)
                plot.getAxis('bottom').setTextPen(fg_color)
                plot.showGrid(x=True, y=True, alpha=grid_alpha/255.0)

            if hasattr(self, 'gl_viewport') and self.gl_viewport is not None:
                self.gl_viewport.set_theme(self.current_theme)

            # Apply Windows DWM dark title bar if running on Windows
            if os.name == 'nt':
                try:
                    hwnd = self.winId().__int__()
                    DWMWA_USE_IMMERSIVE_DARK_MODE = 20
                    set_window_attribute = ctypes.windll.dwmapi.DwmSetWindowAttribute
                    if self.current_theme == 'dark':
                        value = ctypes.c_int(1)
                    else:
                        value = ctypes.c_int(0)
                    set_window_attribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, ctypes.byref(value), ctypes.sizeof(value))
                except Exception as e:
                    self.logger.warning(f"Could not set DWM dark mode: {e}")

            self.update()
            self.repaint()
        except Exception as e:
            print(f"Failed to load stylesheet: {e}")

    def on_task_mode_changed(self, mode):
        self.logger.info(f"切换任务模式: {mode}")
        if hasattr(self, 'sys_values') and "当前任务" in self.sys_values:
             self.sys_values["当前任务"].setText(mode)

        if hasattr(self, 'worker') and self.worker:
            self.worker.set_task_mode(mode)

        if hasattr(self, 'safety_monitor'):
             self.safety_monitor.set_task_mode(mode)
             self.update_alarm_ui(mode)

    def _prepare_3d_model_before_show(self):
        if self.model_rendered or self.model_loading:
            return

        self.model_loading = True
        self.model_preload_started = True
        self.model_pending_show = False
        self.logger.info("开始异步加载 3D 模型")

        try:
            from ..utils.resource import resource_path
            from .widgets.model_loader import ModelLoader

            model_root = resource_path("assets/models")
            result = ModelLoader(model_root=model_root).load_best_available_model()
            self.model_load_result = result

            if result.success:
                self.logger.info(f"3D 模型加载完成，模型类型：{result.model_type}")
            else:
                self.logger.warning(f"3D 模型加载失败：{result.message}")

            if self.gl_viewport is None:
                self.gl_viewport = ModelViewer(
                    model_root=model_root,
                    parent=self.model_stacked_widget,
                    auto_load=False
                )
                self.gl_viewport.set_theme(self.current_theme)
                self.model_stacked_widget.addWidget(self.gl_viewport)

            self.gl_viewport.set_model_result(result)
            self.model_rendered = True
            self.model_loading = False
            self.btn_toggle_3d.setEnabled(True)
            self.btn_toggle_3d.setText("查看 3D 模型")
            self.model_stacked_widget.setCurrentWidget(self.preview_label)
            self.logger.info("3D 模型视图创建并渲染完成")
        except Exception as e:
            self.model_loading = False
            self.model_rendered = False
            self.model_preload_started = False
            self.model_load_result = None
            self.logger.exception(f"3D 模型启动预加载失败: {e}")

    def toggle_3d_view(self):
        if self.model_stacked_widget.currentWidget() == self.preview_label:
            if self.gl_viewport is not None and self.model_rendered:
                self.model_stacked_widget.setCurrentWidget(self.gl_viewport)
                self.btn_toggle_3d.setText("返回图片")
                self.logger.info("3D 模型已缓存，直接显示")
                return

            self._start_3d_model_load(show_when_ready=True)
        else:
            self.model_pending_show = False
            self.model_stacked_widget.setCurrentWidget(self.preview_label)
            self.btn_toggle_3d.setEnabled(True)
            self.btn_toggle_3d.setText("查看 3D 模型")

    def _start_3d_model_load(self, show_when_ready=False):
        if self.model_rendered:
            if show_when_ready and self.gl_viewport is not None:
                self.model_stacked_widget.setCurrentWidget(self.gl_viewport)
                self.btn_toggle_3d.setEnabled(True)
                self.btn_toggle_3d.setText("返回图片")
            return

        self.model_pending_show = self.model_pending_show or show_when_ready

        if show_when_ready:
            self.btn_toggle_3d.setEnabled(False)
            self.btn_toggle_3d.setText("正在加载...")

            if not hasattr(self, "loading_label"):
                self.loading_label = QLabel("正在加载 3D 模型，请稍候...")
                self.loading_label.setAlignment(Qt.AlignCenter)
                self.loading_label.setWordWrap(True)
                self.loading_label.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
                self.model_stacked_widget.addWidget(self.loading_label)
            else:
                self.loading_label.setText("正在加载 3D 模型，请稍候...")
            self.model_stacked_widget.setCurrentWidget(self.loading_label)

        if self.model_loading or self.model_preload_started:
            return

        from ..utils.resource import resource_path
        model_root = resource_path("assets/models")

        self.model_loading = True
        self.model_preload_started = True
        self.logger.info("开始异步加载 3D 模型")

        if not hasattr(self, "model_load_bridge"):
            self.model_load_bridge = ModelLoadBridge(self)
            self.model_load_bridge.model_loaded.connect(self._on_model_loaded)

        import threading

        def load_model():
            from .widgets.model_loader import ModelLoader, ModelLoadResult
            try:
                loader = ModelLoader(model_root=model_root)
                result = loader.load_best_available_model()
            except Exception as exc:
                result = ModelLoadResult(
                    False,
                    "fallback",
                    "",
                    [],
                    f"3D 模型后台加载异常：{exc}",
                    True,
                    0.0,
                    [f"3D 模型后台加载异常：{exc}"],
                )
            self.model_load_bridge.model_loaded.emit(result)

        self.load_thread = threading.Thread(target=load_model, name="ModelLoaderThread", daemon=True)
        self.load_thread.start()

    def _on_model_loaded(self, result):
        self.model_loading = False
        self.model_load_result = result
        if result.success:
            self.logger.info(f"3D 模型加载完成，模型类型：{result.model_type}")
        else:
            self.logger.warning(f"3D 模型加载失败：{result.message}")

        try:
            from ..utils.resource import resource_path
            from PySide6.QtCore import QTimer

            if self.gl_viewport is None:
                model_root = resource_path("assets/models")
                self.gl_viewport = ModelViewer(
                    model_root=model_root,
                    parent=self.model_stacked_widget,
                    auto_load=False
                )
                self.gl_viewport.set_theme(self.current_theme)
                self.model_stacked_widget.addWidget(self.gl_viewport)

            if self.model_pending_show:
                self.model_stacked_widget.setCurrentWidget(self.gl_viewport)

            if self.model_pending_show and hasattr(self, "loading_label"):
                self.loading_label.setText("正在渲染 3D 模型，请稍候...")

            QTimer.singleShot(50, lambda: self._render_loaded_model(result))
        except Exception as e:
            self.model_loading = False
            self.logger.exception(f"3D 视图创建失败: {e}")
            self._show_3d_error_page(e)

    def _render_loaded_model(self, result):
        try:
            if self.gl_viewport is None:
                return

            self.gl_viewport.set_model_result(result)
            self.model_rendered = True

            if self.model_pending_show:
                self.btn_toggle_3d.setEnabled(True)
                self.btn_toggle_3d.setText("返回图片")
                self.model_stacked_widget.setCurrentWidget(self.gl_viewport)
            else:
                self.btn_toggle_3d.setEnabled(True)
                self.btn_toggle_3d.setText("查看 3D 模型")
                if self.model_stacked_widget.currentWidget() != self.preview_label:
                    self.model_stacked_widget.setCurrentWidget(self.preview_label)

            self.logger.info("3D 模型视图创建并渲染完成")
        except Exception as e:
            self.model_loading = False
            self.logger.exception(f"3D 模型渲染失败: {e}")
            self._show_3d_error_page(e)
            self.btn_toggle_3d.setEnabled(True)
            self.btn_toggle_3d.setText("查看 3D 模型")

    def _show_3d_error_page(self, error):
        self.model_loading = False
        self.model_rendered = False
        lbl = QLabel(
            "3D 模型加载失败，请检查模型文件或 OpenGL 环境。\n"
            f"详细错误：{error}"
        )
        lbl.setAlignment(Qt.AlignCenter)
        lbl.setWordWrap(True)
        lbl.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Expanding)
        self.model_stacked_widget.addWidget(lbl)
        if self.model_pending_show:
            self.model_stacked_widget.setCurrentWidget(lbl)
        else:
            self.model_stacked_widget.setCurrentWidget(self.preview_label)
        self.gl_viewport = None

    def toggle_theme(self):
        if self.current_theme == 'light':
            self.current_theme = 'dark'
            self.btn_theme.setText("切换浅色主题")
        else:
            self.current_theme = 'light'
            self.btn_theme.setText("切换深色主题")

        self.apply_theme()

    def on_record_toggled(self, state):
        checked = self.record_checkbox.isChecked()

        if checked:
            self.format_combo.setEnabled(False)
            for btn in self.format_combo._buttons.values(): btn.setEnabled(False)
            self.path_btn.setEnabled(False)

            fmt = self.format_combo.currentText()
            if not self.save_path:
                from ..utils.resource import get_exe_dir
                prefix = "FaWave_RealData" if self.get_worker_mode() == "TCP" else "FaWave_SimData"
                timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
                self.save_path = os.path.join(get_exe_dir(), "Data", f"{prefix}_{timestamp}.{fmt.lower()}")

                # Try preparing the file before starting (handles dynamic directory creation + headers)
                if not self.data_recorder.prepare_file(self.save_path, format=fmt):
                     QMessageBox.warning(self, "保存错误", f"无法创建文件:\n{self.data_recorder.last_error}")
                     self.record_checkbox.setChecked(False)
                     return

            metrics = QFontMetrics(self.path_label.font())
            elided = metrics.elidedText(self.save_path, Qt.ElideMiddle, 300)
            self.path_label.setText(elided)
            self.path_label.setToolTip(self.save_path)

            success = self.data_recorder.start_recording(self.save_path, format=fmt)
            if success:
                self.logger.info(f"用户启用本地存储，开始写入数据: {self.save_path}")
            else:
                self.logger.error(f"启动保存异常: {self.data_recorder.last_error}", exc_info=True)
                QMessageBox.warning(self, "保存错误", f"无法开始记录:\n{self.data_recorder.last_error}")
                self.record_checkbox.setChecked(False)
            self.update_status()
        else:
            fmt = self.format_combo.currentText()
            if fmt == "XLSX" and self.data_recorder.is_recording:
                # Update status bar immediately before the blocking conversion runs
                self.statusBar.showMessage("正在生成 XLSX 文件...", 5000)
                QApplication.processEvents()

            self.data_recorder.stop_recording()

            self.format_combo.setEnabled(True)
            for btn in self.format_combo._buttons.values(): btn.setEnabled(True)
            self.path_btn.setEnabled(True)
            self.logger.info(f"用户停止本地存储，保存已停止: {self.save_path}。 已保存行数: {self.data_recorder.saved_count}")
            if fmt == "XLSX":
                self.statusBar.showMessage("保存完成", 3000)
            self.update_status()

    def select_save_path(self):
        if self.data_recorder.is_recording:
            return

        from ..utils.resource import get_exe_dir
        fmt = self.format_combo.currentText().lower()
        default_dir = os.path.join(get_exe_dir(), "Data")
        os.makedirs(default_dir, exist_ok=True)
        prefix = "FaWave_RealData" if self.get_worker_mode() == "TCP" else "FaWave_SimData"
        timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
        default_name = f"{prefix}_{timestamp}.{fmt}"

        file_path, _ = QFileDialog.getSaveFileName(
            self, "选择保存路径", os.path.join(default_dir, default_name),
            f"数据文件 (*.{fmt})"
        )
        if file_path:
            if not file_path.lower().endswith(f".{fmt}"):
                file_path = f"{os.path.splitext(file_path)[0]}.{fmt}"

            self.save_path = file_path

            # Immediately prepare the file and write headers upon selection
            success = self.data_recorder.prepare_file(self.save_path, format=fmt)

            if success:
                metrics = QFontMetrics(self.path_label.font())
                elided = metrics.elidedText(self.save_path, Qt.ElideMiddle, 300)
                self.path_label.setText(elided)
                self.path_label.setToolTip(self.save_path)
                self.logger.info(f"用户选择保存路径，文件已创建: {self.save_path}")
            else:
                QMessageBox.warning(self, "创建失败", f"无法创建文件:\n{self.data_recorder.last_error}")
                self.save_path = ""
                self.path_label.setText("未选择")
            self.update_status()

    def update_plot_visibility(self):
        self.curve_ch1.setVisible(self.chk_ch1.isChecked())
        self.curve_ch2.setVisible(self.chk_ch2.isChecked())
        self.curve_ch3.setVisible(self.chk_ch3.isChecked())
        self.curve_ch4.setVisible(self.chk_ch4.isChecked())
        self.curve_fx.setVisible(self.chk_fx.isChecked())
        self.curve_fy.setVisible(self.chk_fy.isChecked())
        self.curve_fz.setVisible(self.chk_fz.isChecked())

    def clear_plot(self):
        self.data_buffer.clear()
        self.curve_ch1.setData([], [])
        self.curve_ch2.setData([], [])
        self.curve_ch3.setData([], [])
        self.curve_ch4.setData([], [])

        self.curve_fx.setData([], [])
        self.curve_fy.setData([], [])
        self.curve_fz.setData([], [])

    def zero_force(self):
        if hasattr(self.worker, 'request_force_zero'):
            self.worker.request_force_zero()
            # Status will be updated via the normal UI poll of the worker's status
        else:
            QMessageBox.warning(self, "操作失败", "解耦算法不可用。")

    def on_plot_interacted(self):
        if not self._programmatic_range_update and self.auto_follow:
            self.auto_follow = False
            self.btn_autoscale.setText("自动跟随：关闭")

    def toggle_auto_follow(self):
        self.auto_follow = not self.auto_follow
        if self.auto_follow:
            self.btn_autoscale.setText("自动跟随：开启")
        else:
            self.btn_autoscale.setText("自动跟随：关闭")

    def get_worker_mode(self):
        if self.mode_combo.currentText() == "真实设备":
            return "TCP"
        elif self.mode_combo.currentText() == "仿真演示":
            return "Mock"
        return "TCP"

    def get_display_mode(self):
        return self.mode_combo.currentText()

    def open_advanced_settings(self):
        if self.worker.is_running:
            QMessageBox.warning(self, "运行中", "采集运行中禁止修改高级设置。")
            return

        dialog = AdvancedSettingsDialog(self.config, self)
        if dialog.exec_():
            self.logger.info("更新高级设置")
            # Re-init the decoder based on new config
            self.worker.init_decoder()
            self.sys_values["输入单位"].setText(self.config.get("force_decoder", {}).get("input_unit", "V"))

        # update system status
        self.sys_values["算法状态"].setText("Python解耦运行中" if self.worker.is_running else "Python解耦")
        self.sys_values["解耦状态"].setText("未初始化")

    def reset_connection_ui_after_failure(self):
        self.btn_connect.setText("建立连接")
        self.btn_connect.setObjectName("btnConnect")
        self.apply_theme()

        self.ip_input.setEnabled(True)
        self.port_input.setEnabled(True)
        self.interval_input.setEnabled(True)
        self.mode_combo.setEnabled(True)
        self.btn_advanced.setEnabled(True)
        for btn in self.mode_combo._buttons.values(): btn.setEnabled(True)

        self.status_capsule.setText("● 错误")
        self.status_capsule.setObjectName("statusCapsule_Error")
        self.sys_values["连接状态"].setText("错误")
        self.style().unpolish(self.status_capsule)
        self.style().polish(self.status_capsule)
        self.update_status()
        self.btn_connect.setEnabled(True)

    def toggle_connection(self):
        if not self.worker.is_running:
            # Connect
            ip = self.ip_input.text()
            try:
                port = int(self.port_input.text())
            except ValueError:
                QMessageBox.warning(self, "输入错误", "端口必须是整数。")
                return

            try:
                interval = int(self.interval_input.text())
                self.config["request_interval_ms"] = interval
            except ValueError:
                pass

            worker_mode = self.get_worker_mode()

            self.btn_connect.setText("连接中...")
            self.btn_connect.setEnabled(False)

            self.logger.info(f"连接参数: IP={ip}, Port={port}, Mode={worker_mode}")
            self.worker.set_connection_params(worker_mode, ip, port)
            self.worker.start()

            self.ui_timer.start(self.refresh_rate_ms)

            self.ip_input.setEnabled(False)
            self.port_input.setEnabled(False)
            self.interval_input.setEnabled(False)
            self.mode_combo.setEnabled(False)
            self.btn_advanced.setEnabled(False)
            for btn in self.mode_combo._buttons.values(): btn.setEnabled(False)

        else:
            # Disconnect
            self.logger.info("用户主动断开连接")
            self.worker.stop()

            if self.record_checkbox.isChecked():
                 self.record_checkbox.setChecked(False) # implicitly stops recording
            else:
                 self.data_recorder.stop_recording() # safety catch

            self.ui_timer.stop()

            self.btn_connect.setText("建立连接")
            self.btn_connect.setObjectName("btnConnect")
            self.btn_connect.setEnabled(True)
            self.apply_theme()

            self.ip_input.setEnabled(True)
            self.port_input.setEnabled(True)
            self.interval_input.setEnabled(True)
            self.mode_combo.setEnabled(True)
            self.btn_advanced.setEnabled(True)
            for btn in self.mode_combo._buttons.values(): btn.setEnabled(True)

    def on_connection_status_changed(self, status):
        is_simulation = self.get_display_mode() == "仿真演示"

        if status == "Connected":
            disp_text = "● 仿真运行" if is_simulation else "● 已连接"
            self.status_capsule.setText(disp_text)
            self.status_capsule.setObjectName("statusCapsule_Connected")
            self.sys_values["连接状态"].setText(disp_text.replace("● ", ""))

            self.btn_connect.setText("断开连接")
            self.btn_connect.setObjectName("btnDisconnect")
            self.btn_connect.setEnabled(True)
            self.apply_theme() # Refresh styling

            if not is_simulation:
                QTimer.singleShot(100, lambda: self.statusBar.showMessage("请保持传感器无载静止约 5 秒，用于自动建立三维力基线。", 5000))
        elif status == "Disconnected":
            self.status_capsule.setText("● 未连接")
            self.status_capsule.setObjectName("statusCapsule_Disconnected")
            self.sys_values["连接状态"].setText("未连接")

            self.btn_connect.setText("建立连接")
            self.btn_connect.setObjectName("btnConnect")
            self.btn_connect.setEnabled(True)
            self.apply_theme()
        else:
            self.reset_connection_ui_after_failure()
            return

        self.style().unpolish(self.status_capsule)
        self.style().polish(self.status_capsule)
        self.update_status()

    def on_error_occurred(self, err_msg):
        self.last_error = err_msg
        self.update_status()

    def update_status(self):
        conn_str = self.status_capsule.text().replace("● ", "")

        status = self.data_recorder.get_status()

        save_str = "未保存"
        if status["state"] == "file_prepared":
            save_str = "等待记录"
        elif status["state"] == "recording":
            if status["queued"] > 5000:
                save_str = "写入积压"
            else:
                save_str = "正在保存"
        elif status["state"] == "stopping":
            save_str = "正在停止"
        elif status["state"] == "stopped":
            save_str = "保存已停止"
        elif status["state"] == "error":
            save_str = "保存错误"

        acq_mode = self.get_display_mode()

        run_time_str = "00:00:00"
        if self.worker.is_running and self.worker.start_time > 0:
            import time
            elapsed = int(time.time() - self.worker.start_time)
            run_time_str = f"{elapsed//3600:02d}:{(elapsed%3600)//60:02d}:{elapsed%60:02d}"

        # Update System Status Card
        self.sys_values["采集模式"].setText(acq_mode)
        self.sys_values["有效帧"].setText(str(self.worker.recv_frames))
        self.sys_values["错误帧"].setText(str(self.worker.error_frames))
        self.sys_values["运行时间"].setText(run_time_str)
        self.sys_values["保存状态"].setText(save_str)

        err_display = self.last_error
        if err_display.startswith("最近错误："):
            err_display = err_display.replace("最近错误：", "")
        self.sys_values["最近错误"].setText(err_display)

        # Update bottom status bar
        status_text = (
            f"连接状态：{conn_str} | "
            f"采集模式：{acq_mode} | "
            f"有效帧：{self.worker.recv_frames} | "
            f"错误帧：{self.worker.error_frames} | "
            f"运行时间：{run_time_str} | "
            f"保存状态：{save_str} | "
            f"最近错误：{self.last_error}"
        )
        self.statusBar.showMessage(status_text)

    def auto_range_plot(self, plot_widget, datasets, default_range, padding=0.1):
        if not datasets:
            return

        min_y = float('inf')
        max_y = float('-inf')

        for data in datasets:
            if not data:
                continue
            curr_min = min(data)
            curr_max = max(data)
            if curr_min < min_y:
                min_y = curr_min
            if curr_max > max_y:
                max_y = curr_max

        if min_y == float('inf') or max_y == float('-inf'):
            plot_widget.setYRange(*default_range)
            return

        if max_y - min_y < 0.0001:
            if max_y == 0:
                plot_widget.setYRange(*default_range)
            else:
                margin = abs(max_y) * padding
                plot_widget.setYRange(min_y - margin, max_y + margin)
            return

        range_span = max_y - min_y
        margin = range_span * padding
        plot_widget.setYRange(min_y - margin, max_y + margin)

    def update_ui(self):
        self.update_status()

        t_data, idx_data, ch1, ch2, ch3, ch4, fx, fy, fz, decoder_status, backend, validated, alarms, recent_alarm = self.data_buffer.get_data()

        if alarms:
            for i, card in enumerate(self.alarm_cards):
                if i < len(alarms):
                    cfg = alarms[i]
                    card.set_state(f"● {cfg['event']}", cfg['level'], cfg['reason'])
                    card.show()
                else:
                    card.hide()

        if recent_alarm:
            self.recent_alarm_label.setText(f"{recent_alarm['event']} [{recent_alarm['level']}] - {recent_alarm['ts']/1000.0:.1f}s")

        self.sys_values["解耦状态"].setText(decoder_status)
        # Algorithm state is statically tied to python backend now
        self.f_status.setText(decoder_status)

        if not t_data:
            return

        self.curve_ch1.setData(t_data, ch1)
        self.curve_ch2.setData(t_data, ch2)
        self.curve_ch3.setData(t_data, ch3)
        self.curve_ch4.setData(t_data, ch4)

        self.curve_fx.setData(t_data, fx)
        self.curve_fy.setData(t_data, fy)
        self.curve_fz.setData(t_data, fz)

        if hasattr(self, 'gl_viewport') and self.gl_viewport is not None:
            if hasattr(self, 'model_stacked_widget') and self.model_stacked_widget.currentWidget() == self.gl_viewport:
                if len(fx) > 0 and len(fy) > 0 and len(fz) > 0:
                    self.gl_viewport.update_force_vectors(fx[-1], fy[-1], fz[-1])

        # Apply Auto Follow
        if hasattr(self, 'auto_follow') and self.auto_follow:
            self._programmatic_range_update = True

            t_end = t_data[-1]
            t_start = max(t_data[0], t_end - self.follow_window_s)

            self.plot_voltage.setXRange(t_start, t_end, padding=0)
            self.plot_force.setXRange(t_start, t_end, padding=0)

            # We filter data points to only those within [t_start, t_end]
            # To optimize, we can just use the whole array if it's within deque size
            # Since max_points=2000 at 20ms is 40s, we should slice it.
            try:
                start_idx = next(i for i, t in enumerate(t_data) if t >= t_start)
            except StopIteration:
                start_idx = 0

            # Voltage plot
            active_v_data = []
            if getattr(self, 'chk_ch1', None) and self.chk_ch1.isChecked(): active_v_data.append(ch1[start_idx:])
            if getattr(self, 'chk_ch2', None) and self.chk_ch2.isChecked(): active_v_data.append(ch2[start_idx:])
            if getattr(self, 'chk_ch3', None) and self.chk_ch3.isChecked(): active_v_data.append(ch3[start_idx:])
            if getattr(self, 'chk_ch4', None) and self.chk_ch4.isChecked(): active_v_data.append(ch4[start_idx:])
            self.auto_range_plot(self.plot_voltage, active_v_data, [-2.5, 2.5])

            # Force plot
            active_f_data = []
            if getattr(self, 'chk_fx', None) and self.chk_fx.isChecked(): active_f_data.append(fx[start_idx:])
            if getattr(self, 'chk_fy', None) and self.chk_fy.isChecked(): active_f_data.append(fy[start_idx:])
            if getattr(self, 'chk_fz', None) and self.chk_fz.isChecked(): active_f_data.append(fz[start_idx:])
            self.auto_range_plot(self.plot_force, active_f_data, [-5, 5])

            self._programmatic_range_update = False

        self.card_ch1.set_value(ch1[-1])
        self.card_ch2.set_value(ch2[-1])
        self.card_ch3.set_value(ch3[-1])
        self.card_ch4.set_value(ch4[-1])

        self.card_fx.set_value(fx[-1])
        self.card_fy.set_value(fy[-1])
        self.card_fz.set_value(fz[-1])

        # We need to look up if the buffer holds recent alarms. In a fully optimized flow
        # we would fetch the alarms directly from the worker buffer.
        # But we'll do it safely from the last data point if available, or force a poll.
        pass

    def closeEvent(self, event):
        if self.worker.is_running:
            self.worker.stop()
        self.data_recorder.stop_recording()
        event.accept()
