// Workspace initialization, bridge, and recording
var workspace;
var isRecording = false;
var recordedActions = [];

document.addEventListener('DOMContentLoaded', function() {
  workspace = Blockly.inject('blocklyDiv', {
    toolbox: TOOLBOX,
    grid: { spacing: 20, length: 3, colour: '#313244', snap: true },
    zoom: {
      controls: true, wheel: true,
      startScale: 1.0, maxScale: 3, minScale: 0.3, scaleSpeed: 1.2
    },
    trashcan: true,
    renderer: 'zelos',
    theme: Blockly.Theme.defineTheme('mirage', {
      base: Blockly.Themes.Classic,
      componentStyles: {
        workspaceBackgroundColour: '#1e1e2e',
        toolboxBackgroundColour: '#181825',
        flyoutBackgroundColour: '#313244',
        scrollbarColour: '#45475a',
        insertionMarkerColour: '#89b4fa'
      }
    })
  });

  workspace.addChangeListener(function(event) {
    if (event.isUiEvent) return;
    updateCodePreview();
  });

  window.addEventListener('resize', function() {
    Blockly.svgResize(workspace);
  });

  addRecordButton();
  waitForPywebview();
});

// Wait for pywebview JS bridge to be ready
function waitForPywebview() {
  if (window.pywebview && window.pywebview.api) {
    refreshDevices();
  } else {
    // pywebview fires 'pywebviewready' event when bridge is injected
    window.addEventListener('pywebviewready', function() {
      refreshDevices();
    });
    // Fallback: poll every 500ms for up to 10 seconds
    var attempts = 0;
    var timer = setInterval(function() {
      attempts++;
      if (window.pywebview && window.pywebview.api) {
        clearInterval(timer);
        refreshDevices();
      } else if (attempts > 20) {
        clearInterval(timer);
        console.log('pywebview bridge not available');
      }
    }, 500);
  }
}

function updateCodePreview() {
  try {
    var code = Blockly.Python.workspaceToCode(workspace);
    document.getElementById('code-output').textContent = code || '// ブロックをドラッグして開始';
  } catch(e) {
    document.getElementById('code-output').textContent = 'Error: ' + e.message;
  }
}

// ==================== Device Management ====================
async function refreshDevices() {
  try {
    var devices = await window.pywebview.api.get_devices();
    var sel = document.getElementById('device-select');
    sel.innerHTML = '<option value="">デバイス未選択</option>';
    if (Array.isArray(devices)) {
      devices.forEach(function(d) {
        var opt = document.createElement('option');
        opt.value = d.serial;
        opt.textContent = d.model + ' (' + d.serial + ')';
        sel.appendChild(opt);
      });
    }
  } catch(e) { console.log('Device refresh:', e); }
}

// ==================== Recording (Coordinate Picker) ====================
function addRecordButton() {
  var btn = document.createElement('button');
  btn.id = 'btn-record';
  btn.textContent = '⏺ 録画';
  btn.onclick = toggleRecording;
  btn.style.cssText = 'background:#f38ba8;color:#1e1e2e;font-weight:bold;';
  document.getElementById('toolbar-buttons').insertBefore(
    btn, document.getElementById('btn-run')
  );
}

function toggleRecording() {
  if (isRecording) {
    stopRecording();
  } else {
    startRecording();
  }
}

async function startRecording() {
  var serial = document.getElementById('device-select').value;
  if (!serial) {
    alert('先にデバイスを選択してください');
    return;
  }
  isRecording = true;
  recordedActions = [];
  var btn = document.getElementById('btn-record');
  btn.textContent = '⏹ 停止';
  btn.style.background = '#f38ba8';
  btn.style.animation = 'blink 1s infinite';

  if (!document.getElementById('blink-style')) {
    var style = document.createElement('style');
    style.id = 'blink-style';
    style.textContent = '@keyframes blink { 50% { opacity: 0.5; } }';
    document.head.appendChild(style);
  }

  showScreenPicker(serial);
}

function stopRecording() {
  isRecording = false;
  var btn = document.getElementById('btn-record');
  btn.textContent = '⏺ 録画';
  btn.style.animation = '';

  var picker = document.getElementById('screen-picker-overlay');
  if (picker) picker.remove();

  // Remove click markers
  document.querySelectorAll('.click-marker').forEach(function(m) { m.remove(); });

  if (recordedActions.length > 0) {
    createContainerFromRecording();
  }
}

async function showScreenPicker(serial) {
  try {
    var imgData = await window.pywebview.api.capture_screen(serial);
    if (!imgData || imgData.error) {
      alert('スクリーンショット取得失敗: ' + (imgData ? imgData.error : 'unknown'));
      stopRecording();
      return;
    }
    openPickerModal(imgData, serial);
  } catch(e) {
    alert('エラー: ' + e);
    stopRecording();
  }
}

function openPickerModal(imgData, serial) {
  var old = document.getElementById('screen-picker-overlay');
  if (old) old.remove();

  var overlay = document.createElement('div');
  overlay.id = 'screen-picker-overlay';
  overlay.style.cssText = 'position:fixed;top:0;left:0;width:100%;height:100%;'
    + 'background:rgba(0,0,0,0.85);z-index:9999;display:flex;flex-direction:column;'
    + 'align-items:center;padding:20px;';

  var header = document.createElement('div');
  header.style.cssText = 'color:#cdd6f4;margin-bottom:10px;font-size:14px;display:flex;gap:16px;align-items:center;';
  header.innerHTML = '<span>🎯 画面をクリックして座標を記録</span>'
    + '<span id="picker-count" style="color:#a6e3a1;">操作: 0件</span>'
    + '<button onclick="refreshScreenPicker(\'' + serial + '\')" style="padding:4px 12px;border:none;border-radius:4px;background:#89b4fa;color:#1e1e2e;cursor:pointer;">🔄 更新</button>'
    + '<select id="picker-action" style="padding:4px 8px;border-radius:4px;border:1px solid #45475a;background:#313244;color:#cdd6f4;">'
    + '<option value="tap">タップ</option>'
    + '<option value="swipe_start">スワイプ始点</option>'
    + '<option value="long_press">ロングプレス</option>'
    + '</select>'
    + '<button onclick="stopRecording()" style="padding:4px 12px;border:none;border-radius:4px;background:#f38ba8;color:#1e1e2e;cursor:pointer;">✅ 完了</button>';
  overlay.appendChild(header);

  var imgContainer = document.createElement('div');
  imgContainer.style.cssText = 'position:relative;cursor:crosshair;max-height:calc(100vh - 80px);';
  
  var img = document.createElement('img');
  img.id = 'picker-image';
  img.src = 'data:image/png;base64,' + imgData.base64;
  img.style.cssText = 'max-height:calc(100vh - 80px);max-width:90vw;object-fit:contain;';
  img.draggable = false;

  img.onclick = function(e) {
    if (!isRecording) return;
    var rect = img.getBoundingClientRect();
    var scaleX = imgData.width / rect.width;
    var scaleY = imgData.height / rect.height;
    var realX = Math.round((e.clientX - rect.left) * scaleX);
    var realY = Math.round((e.clientY - rect.top) * scaleY);

    var action = document.getElementById('picker-action').value;
    handlePickerClick(realX, realY, action, e);
  };

  imgContainer.appendChild(img);
  overlay.appendChild(imgContainer);
  document.body.appendChild(overlay);
}

var swipeStart = null;

function handlePickerClick(x, y, action, event) {
  if (action === 'tap') {
    recordedActions.push({type: 'tap', x: x, y: y});
    showClickMarker(event.clientX, event.clientY, 'TAP');
  } else if (action === 'long_press') {
    recordedActions.push({type: 'long_press', x: x, y: y, duration: 1000});
    showClickMarker(event.clientX, event.clientY, 'LONG');
  } else if (action === 'swipe_start') {
    if (!swipeStart) {
      swipeStart = {x: x, y: y};
      showClickMarker(event.clientX, event.clientY, 'S1');
      var sel = document.getElementById('picker-action');
      if (!sel.querySelector('option[value="swipe_end"]')) {
        var opt = document.createElement('option');
        opt.value = 'swipe_end';
        opt.textContent = 'スワイプ終点';
        sel.appendChild(opt);
      }
      sel.value = 'swipe_end';
    }
  } else if (action === 'swipe_end') {
    if (swipeStart) {
      recordedActions.push({
        type: 'swipe', x1: swipeStart.x, y1: swipeStart.y,
        x2: x, y2: y, duration: 300
      });
      showClickMarker(event.clientX, event.clientY, 'S2');
      swipeStart = null;
      var sel = document.getElementById('picker-action');
      var endOpt = sel.querySelector('option[value="swipe_end"]');
      if (endOpt) endOpt.remove();
      sel.value = 'tap';
    }
  }

  var counter = document.getElementById('picker-count');
  if (counter) counter.textContent = '操作: ' + recordedActions.length + '件';
}

function showClickMarker(cx, cy, label) {
  var marker = document.createElement('div');
  marker.className = 'click-marker';
  marker.style.cssText = 'position:fixed;left:' + (cx - 12) + 'px;top:' + (cy - 12) + 'px;'
    + 'width:24px;height:24px;border-radius:50%;background:rgba(166,227,161,0.7);'
    + 'border:2px solid #a6e3a1;z-index:10000;display:flex;align-items:center;justify-content:center;'
    + 'font-size:9px;color:#1e1e2e;font-weight:bold;pointer-events:none;';
  marker.textContent = label;
  document.body.appendChild(marker);
  setTimeout(function() { marker.style.opacity = '0.3'; }, 1000);
}

async function refreshScreenPicker(serial) {
  try {
    var imgData = await window.pywebview.api.capture_screen(serial);
    if (imgData && !imgData.error) {
      var img = document.getElementById('picker-image');
      if (img) img.src = 'data:image/png;base64,' + imgData.base64;
    }
  } catch(e) { console.log('Refresh failed:', e); }
}

// ==================== Container Creation ====================
function createContainerFromRecording() {
  var name = prompt('コンテナ名を入力:', '録画_' + new Date().toLocaleTimeString());
  if (!name) name = '録画結果';

  var containerBlock = workspace.newBlock('adb_container');
  containerBlock.setFieldValue(name, 'NAME');
  containerBlock.initSvg();

  var prevBlock = null;
  recordedActions.forEach(function(action) {
    var block = null;
    if (action.type === 'tap') {
      block = workspace.newBlock('adb_tap');
      block.setFieldValue(action.x, 'X');
      block.setFieldValue(action.y, 'Y');
    } else if (action.type === 'swipe') {
      block = workspace.newBlock('adb_swipe');
      block.setFieldValue(action.x1, 'X1');
      block.setFieldValue(action.y1, 'Y1');
      block.setFieldValue(action.x2, 'X2');
      block.setFieldValue(action.y2, 'Y2');
      block.setFieldValue(action.duration, 'DURATION');
    } else if (action.type === 'long_press') {
      block = workspace.newBlock('adb_long_press');
      block.setFieldValue(action.x, 'X');
      block.setFieldValue(action.y, 'Y');
      block.setFieldValue(action.duration, 'DURATION');
    }

    if (block) {
      block.initSvg();
      if (!prevBlock) {
        containerBlock.getInput('STEPS').connection.connect(block.previousConnection);
      } else {
        prevBlock.nextConnection.connect(block.previousConnection);
      }
      prevBlock = block;
    }
  });

  containerBlock.moveTo(new Blockly.utils.Coordinate(50, 50));
  containerBlock.render();
  containerBlock.setCollapsed(true);
  workspace.render();
  recordedActions = [];
}

// ==================== Save / Load / Export ====================
async function saveMacro() {
  var name = prompt('マクロ名を入力:', 'my_macro');
  if (!name) return;
  var ws = Blockly.serialization.workspaces.save(workspace);
  var code = Blockly.Python.workspaceToCode(workspace);
  var r = await window.pywebview.api.save_macro(name, ws, code);
  alert('保存完了: ' + r.path);
}

async function loadMacro() {
  var macros = await window.pywebview.api.list_macros();
  if (!macros || macros.length === 0) { alert('保存マクロなし'); return; }
  var name = prompt('読み込むマクロ:\n' + macros.join('\n'));
  if (!name) return;
  var ws = await window.pywebview.api.load_macro(name);
  if (ws) Blockly.serialization.workspaces.load(ws, workspace);
}

function runMacro() { alert('実行機能はPhase4で実装予定'); }

function exportCode() {
  var code = Blockly.Python.workspaceToCode(workspace);
  var blob = new Blob([code], {type: 'text/plain'});
  var a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'macro.py';
  a.click();
}
