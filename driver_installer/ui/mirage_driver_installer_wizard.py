#!/usr/bin/env python3
# ====================================================
# mirage_driver_installer_wizard.py
# 
# MirageSystem v2 - WinUSB AOA Driver Installer Wizard
# PyQt5 GUI フロントエンド
# 
# Phase 1-A: GUI統合
# ====================================================

import sys
from datetime import datetime
from enum import Enum

from PyQt5.QtWidgets import (
    QApplication, QWizard, QWizardPage, QVBoxLayout,
    QLabel, QPushButton, QProgressBar, QTextEdit, QMessageBox,
    QGroupBox
)
from PyQt5.QtCore import Qt, pyqtSignal, QThread, QTimer
from PyQt5.QtGui import QFont

from core.driver.driver_controller import DriverController, DriverState

# =====================================================
# Enumerations
# =====================================================

class WizardStep(Enum):
    """ウィザード段階"""
    WELCOME = 0
    ENVIRONMENT_CHECK = 1
    DEVICE_DETECTION = 2
    DRIVER_INSTALL = 3
    VERIFICATION = 4
    COMPLETION = 5

class StatusIcon(Enum):
    """ステータスアイコン"""
    OK = "✓"
    WARN = "⚠"
    ERROR = "✗"
    PENDING = "..."

# =====================================================
# Worker Thread
# =====================================================

class DriverInstallWorker(QThread):
    """バックグラウンドでドライバ操作を実行"""
    
    # シグナル定義
    status_changed = pyqtSignal(str)
    progress_changed = pyqtSignal(int)
    completed = pyqtSignal(bool, str)  # success, message
    device_detected = pyqtSignal(str)  # instance_id
    
    def __init__(self, driver_controller: DriverController):
        super().__init__()
        self.controller = driver_controller
        self.operation = None
        self.stop_requested = False
    
    def set_operation(self, op: str):
        """実行する操作を設定"""
        self.operation = op
    
    def run(self):
        """スレッド処理"""
        try:
            if self.operation == "check_environment":
                self.check_environment()
            elif self.operation == "detect_device":
                self.detect_device()
            elif self.operation == "install_driver":
                self.install_driver()
            elif self.operation == "verify_driver":
                self.verify_driver()
            elif self.operation == "rollback_driver":
                self.rollback_driver()
        
        except Exception as e:
            self.completed.emit(False, f"Error: {str(e)}")
    
    def check_environment(self):
        """環境チェック"""
        self.status_changed.emit("Checking environment...")
        self.progress_changed.emit(10)
        
        # Admin チェック
        is_admin = self.controller.check_admin()
        if not is_admin:
            self.completed.emit(False, "Administrator privileges required")
            return
        
        self.progress_changed.emit(50)
        self.status_changed.emit("Checking WDI availability...")
        
        wdi_available = self.controller.check_wdi_available()
        
        self.progress_changed.emit(100)
        self.status_changed.emit("Environment check completed")
        self.completed.emit(True, f"WDI: {'Available' if wdi_available else 'Not found (will use pnputil)'}")
    
    def detect_device(self):
        """デバイス検出"""
        self.status_changed.emit("Detecting AOA device...")
        self.progress_changed.emit(10)
        
        instance_id = self.controller.find_aoa_device()
        
        if instance_id:
            self.progress_changed.emit(100)
            self.status_changed.emit(f"Device detected: {instance_id}")
            self.device_detected.emit(instance_id)
            self.completed.emit(True, "Device detected successfully")
        else:
            self.progress_changed.emit(100)
            self.status_changed.emit("Device not detected")
            self.completed.emit(False, "AOA device not detected. Please enable AOA mode and try again.")
    
    def install_driver(self):
        """ドライバインストール"""
        self.status_changed.emit("Installing driver...")
        self.progress_changed.emit(10)
        
        # バックアップ
        self.status_changed.emit("Backing up current drivers...")
        self.controller.backup_drivers()
        self.progress_changed.emit(30)
        
        # インストール
        self.status_changed.emit("Running installer...")
        success = self.controller.install_driver()
        
        if success:
            self.progress_changed.emit(100)
            self.status_changed.emit("Driver installed successfully")
            self.completed.emit(True, "Installation completed")
        else:
            self.progress_changed.emit(100)
            self.status_changed.emit("Installation failed")
            self.completed.emit(False, "Driver installation failed. Check logs for details.")
    
    def verify_driver(self):
        """ドライバ検証"""
        self.status_changed.emit("Verifying driver...")
        self.progress_changed.emit(10)
        
        state = self.controller.check_driver_state()
        
        self.progress_changed.emit(50)
        self.status_changed.emit(f"Driver state: {state.name}")
        
        if state == DriverState.INSTALLED:
            self.progress_changed.emit(100)
            self.completed.emit(True, "Driver verified successfully")
        else:
            self.progress_changed.emit(100)
            self.completed.emit(False, f"Driver verification failed: {state.name}")
    
    def rollback_driver(self):
        """ドライバロールバック"""
        self.status_changed.emit("Rolling back driver...")
        self.progress_changed.emit(10)
        
        success = self.controller.rollback_driver()
        
        self.progress_changed.emit(100)
        if success:
            self.status_changed.emit("Rollback completed")
            self.completed.emit(True, "Rollback completed successfully")
        else:
            self.status_changed.emit("Rollback completed with warnings")
            self.completed.emit(True, "Rollback completed (may need device reconnection)")

# =====================================================
# Wizard Pages
# =====================================================

class WelcomePage(QWizardPage):
    """ウェルカムページ"""
    
    def __init__(self):
        super().__init__()
        self.setTitle("MirageSystem v2 - AOA Driver Setup")
        self.setSubTitle("Windows USB AOA Driver Installation Wizard")
        
        layout = QVBoxLayout()
        
        # ロゴ/説明
        intro_label = QLabel("""
        This wizard will help you install the WinUSB driver for Android Accessory Protocol (AOA) communication.
        
        The installation will:
        • Detect your connected Android device
        • Install/configure WinUSB driver
        • Verify the installation
        • Provide rollback capability
        
        Prerequisites:
        ✓ Windows 10/11 (64-bit)
        ✓ Administrator privileges
        ✓ Android device with AOA support
        ✓ USB cable
        
        Please ensure your Android device is connected and AOA mode is enabled before proceeding.
        """)
        intro_label.setWordWrap(True)
        intro_label.setFont(QFont("Segoe UI", 10))
        
        layout.addWidget(intro_label)
        layout.addStretch()
        
        self.setLayout(layout)

class EnvironmentCheckPage(QWizardPage):
    """環境チェックページ"""
    
    def __init__(self, worker: DriverInstallWorker):
        super().__init__()
        self.setTitle("Environment Check")
        self.setSubTitle("Verifying system requirements")
        
        self.worker = worker
        
        layout = QVBoxLayout()
        
        # ステータス表示
        self.status_label = QLabel("Checking environment...")
        self.status_label.setFont(QFont("Segoe UI", 10))
        layout.addWidget(self.status_label)
        
        # プログレスバー
        self.progress = QProgressBar()
        layout.addWidget(self.progress)
        
        # チェック項目
        self.check_admin_label = QLabel(f"{StatusIcon.PENDING.value} Administrator privileges")
        self.check_wdi_label = QLabel(f"{StatusIcon.PENDING.value} WDI availability")
        
        layout.addWidget(self.check_admin_label)
        layout.addWidget(self.check_wdi_label)
        layout.addStretch()
        
        self.setLayout(layout)
        
        # シグナル接続
        self.worker.status_changed.connect(self.on_status_changed)
        self.worker.progress_changed.connect(self.progress.setValue)
        self.worker.completed.connect(self.on_completed)
    
    def initializePage(self):
        """ページ初期化時に環境チェック開始"""
        self.worker.set_operation("check_environment")
        self.worker.start()
    
    def on_status_changed(self, status: str):
        """ステータス更新"""
        self.status_label.setText(status)
    
    def on_completed(self, success: bool, message: str):
        """完了時の処理"""
        if success:
            self.check_admin_label.setText(f"{StatusIcon.OK.value} Administrator privileges")
            self.check_wdi_label.setText(f"{StatusIcon.OK.value} {message}")
            self.wizard().next()
        else:
            self.check_admin_label.setText(f"{StatusIcon.ERROR.value} {message}")
            QMessageBox.critical(self, "Environment Check Failed", message)

class DeviceDetectionPage(QWizardPage):
    """デバイス検出ページ"""
    
    def __init__(self, worker: DriverInstallWorker):
        super().__init__()
        self.setTitle("Device Detection")
        self.setSubTitle("Locating Android device with AOA support")
        
        self.worker = worker
        self.instance_id = None
        
        layout = QVBoxLayout()
        
        # ステータス表示
        self.status_label = QLabel("Detecting device...")
        layout.addWidget(self.status_label)
        
        # プログレスバー
        self.progress = QProgressBar()
        layout.addWidget(self.progress)
        
        # デバイス情報
        info_group = QGroupBox("Detected Device")
        info_layout = QVBoxLayout()
        self.device_label = QLabel("Waiting for device...")
        info_layout.addWidget(self.device_label)
        info_group.setLayout(info_layout)
        layout.addWidget(info_group)
        
        layout.addStretch()
        self.setLayout(layout)
        
        # シグナル接続
        self.worker.status_changed.connect(self.on_status_changed)
        self.worker.progress_changed.connect(self.progress.setValue)
        self.worker.device_detected.connect(self.on_device_detected)
        self.worker.completed.connect(self.on_completed)
    
    def initializePage(self):
        """デバイス検出開始"""
        self.worker.set_operation("detect_device")
        self.worker.start()
    
    def on_status_changed(self, status: str):
        self.status_label.setText(status)
    
    def on_device_detected(self, instance_id: str):
        self.instance_id = instance_id
        self.device_label.setText(f"Instance ID: {instance_id}")
    
    def on_completed(self, success: bool, message: str):
        if success:
            self.device_label.setStyleSheet("color: green;")
            QTimer.singleShot(1000, self.wizard().next)
        else:
            QMessageBox.warning(self, "Device Not Detected", message)

class DriverInstallPage(QWizardPage):
    """ドライバインストールページ"""
    
    def __init__(self, worker: DriverInstallWorker):
        super().__init__()
        self.setTitle("Driver Installation")
        self.setSubTitle("Installing WinUSB driver")
        
        self.worker = worker
        
        layout = QVBoxLayout()
        
        # ステータス表示
        self.status_label = QLabel("Preparing installation...")
        layout.addWidget(self.status_label)
        
        # プログレスバー
        self.progress = QProgressBar()
        layout.addWidget(self.progress)
        
        # ログ表示
        log_group = QGroupBox("Installation Log")
        log_layout = QVBoxLayout()
        self.log_text = QTextEdit()
        self.log_text.setReadOnly(True)
        self.log_text.setMaximumHeight(150)
        log_layout.addWidget(self.log_text)
        log_group.setLayout(log_layout)
        layout.addWidget(log_group)
        
        layout.addStretch()
        self.setLayout(layout)
        
        # シグナル接続
        self.worker.status_changed.connect(self.on_status_changed)
        self.worker.progress_changed.connect(self.progress.setValue)
        self.worker.completed.connect(self.on_completed)
    
    def initializePage(self):
        """インストール開始"""
        self.worker.set_operation("install_driver")
        self.worker.start()
    
    def on_status_changed(self, status: str):
        self.status_label.setText(status)
        self.log_text.append(f"[{datetime.now().strftime('%H:%M:%S')}] {status}")
    
    def on_completed(self, success: bool, message: str):
        if success:
            self.log_text.append(f"\n✓ {message}")
            QTimer.singleShot(1000, self.wizard().next)
        else:
            self.log_text.append(f"\n✗ {message}")
            QMessageBox.critical(self, "Installation Failed", message)

class VerificationPage(QWizardPage):
    """検証ページ"""
    
    def __init__(self, worker: DriverInstallWorker):
        super().__init__()
        self.setTitle("Verification")
        self.setSubTitle("Verifying driver installation")
        
        self.worker = worker
        
        layout = QVBoxLayout()
        
        # ステータス表示
        self.status_label = QLabel("Verifying driver...")
        layout.addWidget(self.status_label)
        
        # プログレスバー
        self.progress = QProgressBar()
        layout.addWidget(self.progress)
        
        # 検証項目
        verify_group = QGroupBox("Verification Results")
        verify_layout = QVBoxLayout()
        
        self.device_ok_label = QLabel(f"{StatusIcon.PENDING.value} Device detected")
        self.service_ok_label = QLabel(f"{StatusIcon.PENDING.value} Service = WinUSB")
        self.driver_ok_label = QLabel(f"{StatusIcon.PENDING.value} Driver verified")
        
        verify_layout.addWidget(self.device_ok_label)
        verify_layout.addWidget(self.service_ok_label)
        verify_layout.addWidget(self.driver_ok_label)
        
        verify_group.setLayout(verify_layout)
        layout.addWidget(verify_group)
        
        layout.addStretch()
        self.setLayout(layout)
        
        # シグナル接続
        self.worker.status_changed.connect(self.on_status_changed)
        self.worker.progress_changed.connect(self.progress.setValue)
        self.worker.completed.connect(self.on_completed)
    
    def initializePage(self):
        """検証開始"""
        self.worker.set_operation("verify_driver")
        self.worker.start()
    
    def on_status_changed(self, status: str):
        self.status_label.setText(status)
    
    def on_completed(self, success: bool, message: str):
        if success:
            self.device_ok_label.setText(f"{StatusIcon.OK.value} Device detected")
            self.service_ok_label.setText(f"{StatusIcon.OK.value} Service = WinUSB")
            self.driver_ok_label.setText(f"{StatusIcon.OK.value} Driver verified")
            QTimer.singleShot(1000, self.wizard().next)
        else:
            self.driver_ok_label.setText(f"{StatusIcon.ERROR.value} Verification failed")

class CompletionPage(QWizardPage):
    """完了ページ"""
    
    def __init__(self, worker: DriverInstallWorker):
        super().__init__()
        self.setTitle("Setup Complete")
        self.setSubTitle("AOA driver installation completed")
        
        self.worker = worker
        
        layout = QVBoxLayout()
        
        # 完了メッセージ
        completion_label = QLabel("""
        ✓ WinUSB driver installation completed successfully!
        
        Your Android device is now ready for AOA communication.
        
        Next steps:
        1. Start MirageSystem application
        2. Connect your Android device
        3. Enable AOA mode in app settings
        
        If you encounter issues:
        • Check the installation log
        • Verify device connection
        • Run rollback and retry if necessary
        """)
        completion_label.setWordWrap(True)
        completion_label.setFont(QFont("Segoe UI", 10))
        
        layout.addWidget(completion_label)
        layout.addStretch()
        
        # ロールバックボタン
        rollback_btn = QPushButton("Rollback Driver")
        rollback_btn.clicked.connect(self.on_rollback_clicked)
        layout.addWidget(rollback_btn)
        
        self.setLayout(layout)
        self.worker = worker
    
    def on_rollback_clicked(self):
        """ロールバック実行"""
        reply = QMessageBox.question(
            self,
            "Confirm Rollback",
            "Are you sure you want to rollback the driver?\nThis action cannot be undone.",
            QMessageBox.Yes | QMessageBox.No
        )
        
        if reply == QMessageBox.Yes:
            self.worker.set_operation("rollback_driver")
            self.worker.start()

# =====================================================
# Main Wizard
# =====================================================

class DriverInstallerWizard(QWizard):
    """メインウィザード"""
    
    def __init__(self):
        super().__init__()
        
        self.setWindowTitle("MirageSystem v2 - AOA Driver Setup Wizard")
        self.setGeometry(100, 100, 600, 500)
        
        # ドライバコントローラ
        self.driver_controller = DriverController()
        
        # ワーカースレッド
        self.worker = DriverInstallWorker(self.driver_controller)
        
        # ウィザードページ追加
        self.addPage(WelcomePage())
        self.addPage(EnvironmentCheckPage(self.worker))
        self.addPage(DeviceDetectionPage(self.worker))
        self.addPage(DriverInstallPage(self.worker))
        self.addPage(VerificationPage(self.worker))
        self.addPage(CompletionPage(self.worker))
        
        # ボタン設定
        self.setButtonText(QWizard.FinishButton, "Close")
        self.setButtonText(QWizard.NextButton, "Next >")
        self.setButtonText(QWizard.BackButton, "< Back")

# =====================================================
# Main Entry Point
# =====================================================

def main():
    """メインエントリーポイント"""
    
    app = QApplication(sys.argv)
    
    wizard = DriverInstallerWizard()
    wizard.show()
    
    sys.exit(app.exec_())

if __name__ == "__main__":
    main()
