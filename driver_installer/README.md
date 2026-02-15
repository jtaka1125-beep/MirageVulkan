# MirageUnified (MirageSystem v2 + WinUSB AOA Driver Stack)

This tree is a unified, single-source-of-truth layout that combines:
- CLI driver orchestrator (PowerShell/pnputil/WDI)
- GUI installer wizard (PyQt5)
- Operational docs (signature, deployment, test report)

## Entry points

### GUI
```bash
pip install PyQt5
python -m ui.mirage_driver_installer_wizard
```

### CLI
```bash
python -m core.driver.setup_orchestrator
```

## Notes
- `core/driver/setup_orchestrator.py` is the only orchestrator implementation.
- `core/driver/driver_controller.py` is the GUI-facing facade.
- Signature strategy and troubleshooting: see `docs/SIGNATURE_OPERATIONAL_GUIDE.md`.
