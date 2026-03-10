// Workspace initialization, bridge, recording, live test, device monitor
var workspace;
var isRecording = false;
var recordedActions = [];
var isMacroRunning = false;
var deviceMonitorTimer = null;
var mirageConnected = false;

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
    refreshNormalizationIndicators();
  });

  window.addEventListener('resize', function() {
    Blockly.svgResize(workspace);
  });

  addRecordButton();
  registerContextMenu();
  waitForPywebview();
});


function isTouchBlockNeedingNormalization(block) {
  return block && (block.type === 'adb_tap' || block.type === 'adb_swipe' || block.type === 'adb_long_press');
}

function hasNormalizedCoordData(block) {
  if (!block || !block.data) return false;
  try {
    var meta = JSON.parse(block.data);
    return meta && meta.coord_basis === 'native_normalized';
  } catch (e) {
    return false;
  }
}

function refreshNormalizationIndicators() {
  if (!workspace) return;
  const blocks = workspace.getAllBlocks(false);
  for (const block of blocks) {
    if (!isTouchBlockNeedingNormalization(block)) continue;
    if (hasNormalizedCoordData(block)) {
      block.setWarningText(null);
      block.setCommentText('normalized');
    } else {
      block.setCommentText(null);
      block.setWarningText('座標未正規化');
    }
  }
}

// Wait for pywebview JS bridge to be ready
function waitForPywebview() {
  if (window.pywebview && window.pywebview.api) {
    onBridgeReady();
  } else {
    window.addEventListener('pywebviewready', function() {
      onBridgeReady();
    });
    var attempts = 0;
    var timer = setInterval(function() {
      attempts++;
      if (window.pywebview && window.pywebview.api) {
        clearInterval(timer);
        onBridgeReady();
      } else if (attempts > 20) {
        clearInterval(timer);
        console.log('pywebview bridge not available');
      }
    }, 500);
  }
}

function onBridgeReady() {
  refreshDevices();
  startDeviceMonitor();
  refreshNormalizationIndicators();
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
    var currentVal = sel.value;
    sel.innerHTML = '<option value="">デバイス未選択</option>';
    if (Array.isArray(devices)) {
      devices.forEach(function(d) {
        var opt = document.createElement('option');
        opt.value = d.serial;
        opt.textContent = d.model + ' (' + d.serial + ')';
        if (d.source === 'mirage') opt.textContent += ' 🔗';
        sel.appendChild(opt);
      });
      // Restore previous selection if still present
      if (currentVal) {
        for (var i = 0; i < sel.options.length; i++) {
          if (sel.options[i].value === currentVal) { sel.value = currentVal; break; }
        }
      }
    }
  } catch(e) { console.log('Device refresh:', e); }
}

// ==================== Device Monitor (polling) ====================
function startDeviceMonitor() {
  if (deviceMonitorTimer) clearInterval(deviceMonitorTimer);
  updateConnectionStatus();
  deviceMonitorTimer = setInterval(function() {
    refreshDevices();
    updateConnectionStatus();
  }, 5000);
}

async function updateConnectionStatus() {
  var indicator = document.getElementById('mirage-status');
  if (!indicator) {
    indicator = document.createElement('span');
    indicator.id = 'mirage-status';
    indicator.style.cssText = 'font-size:12px;padding:4px 8px;border-radius:4px;cursor:pointer;user-select:none;';
    indicator.title = 'MirageGUI接続状態（クリックでリフレッシュ）';
    indicator.onclick = function() { refreshDevices(); updateConnectionStatus(); };
    var toolbar = document.getElementById('toolbar-buttons');
    toolbar.insertBefore(indicator, toolbar.firstChild);
  }

  try {
    var result = await window.pywebview.api.ping();
    mirageConnected = result && result.mirage_connected;
    if (mirageConnected) {
      indicator.textContent = '🟢 MirageGUI';
      indicator.style.background = 'rgba(166,227,161,0.15)';
      indicator.style.color = '#a6e3a1';
    } else {
      indicator.textContent = '🟡 ADB直接';
      indicator.style.background = 'rgba(249,226,175,0.15)';
      indicator.style.color = '#f9e2af';
    }
  } catch(e) {
    mirageConnected = false;
    indicator.textContent = '🔴 未接続';
    indicator.style.background = 'rgba(243,139,168,0.15)';
    indicator.style.color = '#f38ba8';
  }
}

// ==================== Right-Click Context Menu (Live Test) ====================
function registerContextMenu() {
  // "⚡ このブロックを実行" menu item
  Blockly.ContextMenuRegistry.registry.register({
    displayText: '⚡ このブロックを実行',
    preconditionFn: function(scope) {
      if (!scope.block) return 'hidden';
      // Only show on action blocks (not containers, not variables)
      var type = scope.block.type;
      if (type.startsWith('adb_') || type.startsWith('mirage_')) return 'enabled';
      return 'hidden';
    },
    callback: function(scope) {
      executeBlockLive(scope.block);
    },
    scopeType: Blockly.ContextMenuRegistry.ScopeType.BLOCK,
    id: 'execute_block_live',
    weight: 0
  });

  // "⚡ ここから実行" - execute from this block downward
  Blockly.ContextMenuRegistry.registry.register({
    displayText: '⚡ ここから下を実行',
    preconditionFn: function(scope) {
      if (!scope.block) return 'hidden';
      if (scope.block.type.startsWith('adb_') && scope.block.nextConnection) return 'enabled';
      return 'hidden';
    },
    callback: function(scope) {
      executeFromBlockLive(scope.block);
    },
    scopeType: Blockly.ContextMenuRegistry.ScopeType.BLOCK,
    id: 'execute_from_block',
    weight: 1
  });
}

async function executeBlockLive(block) {
  var serial = document.getElementById('device-select').value;
  if (!serial) { alert('先にデバイスを選択してください'); return; }

  var code = generateCodeForBlock(block);
  if (!code || !code.trim()) { alert('このブロックからコードを生成できません'); return; }

  // Visual feedback - highlight block
  block.setHighlighted(true);
  showRunLog(['⚡ 即時実行: ' + block.type], 'running');

  try {
    var result = await window.pywebview.api.run_macro(serial, code);
    block.setHighlighted(false);

    if (result.status === 'ok') {
      flashBlock(block, '#a6e3a1');  // Green flash
      var logLines = result.log || [];
      logLines.push('✅ 完了');
      showRunLog(logLines, 'success');
    } else {
      flashBlock(block, '#f38ba8');  // Red flash
      showRunLog(result.log || ['❌ ' + (result.error || 'エラー')], 'error');
    }
  } catch(e) {
    block.setHighlighted(false);
    flashBlock(block, '#f38ba8');
    showRunLog(['❌ ' + e], 'error');
  }
}

async function executeFromBlockLive(startBlock) {
  var serial = document.getElementById('device-select').value;
  if (!serial) { alert('先にデバイスを選択してください'); return; }

  // Collect code from this block and all following
  var code = '';
  var block = startBlock;
  var blocks = [];
  while (block) {
    blocks.push(block);
    code += generateCodeForBlock(block);
    block = block.getNextBlock();
  }

  if (!code.trim()) { alert('実行するコードがありません'); return; }

  // Highlight all blocks in chain
  blocks.forEach(function(b) { b.setHighlighted(true); });
  showRunLog(['⚡ ' + blocks.length + 'ブロック実行中...'], 'running');

  try {
    var result = await window.pywebview.api.run_macro(serial, code);
    blocks.forEach(function(b) { b.setHighlighted(false); });

    if (result.status === 'ok') {
      blocks.forEach(function(b) { flashBlock(b, '#a6e3a1'); });
      var logLines = result.log || [];
      logLines.push('✅ ' + blocks.length + 'ブロック完了');
      showRunLog(logLines, 'success');
    } else {
      showRunLog(result.log || ['❌ ' + (result.error || 'エラー')], 'error');
    }
  } catch(e) {
    blocks.forEach(function(b) { b.setHighlighted(false); });
    showRunLog(['❌ ' + e], 'error');
  }
}

function generateCodeForBlock(block) {
  // Generate Python code for a single block using Blockly.Python
  try {
    var generator = Blockly.Python;
    if (generator.forBlock && generator.forBlock[block.type]) {
      var result = generator.forBlock[block.type](block, generator);
      if (Array.isArray(result)) return result[0]; // [code, order]
      return result || '';
    }
    // Fallback: generate for whole workspace and extract (less precise)
    return '';
  } catch(e) {
    console.log('Code gen error for ' + block.type + ':', e);
    return '';
  }
}

function flashBlock(block, color) {
  // Brief color flash on block SVG
  var svg = block.getSvgRoot();
  if (!svg) return;
  var origFilter = svg.style.filter;
  svg.style.filter = 'drop-shadow(0 0 8px ' + color + ')';
  setTimeout(function() { svg.style.filter = origFilter; }, 800);
}

// ==================== Macro Execution ====================
async function runMacro() {
  var serial = document.getElementById('device-select').value;
  if (!serial) {
    alert('先にデバイスを選択してください');
    return;
  }
  var code = Blockly.Python.workspaceToCode(workspace);
  if (!code || !code.trim()) {
    alert('実行するブロックがありません');
    return;
  }

  isMacroRunning = true;
  var runBtn = document.getElementById('btn-run');
  var stopBtn = document.getElementById('btn-stop');
  runBtn.style.display = 'none';
  stopBtn.style.display = 'inline-block';
  showRunLog(['⏳ 実行開始...'], 'running');

  try {
    var result = await window.pywebview.api.run_macro(serial, code);
    isMacroRunning = false;
    runBtn.style.display = 'inline-block';
    stopBtn.style.display = 'none';

    var logLines = result.log || [];
    var status = result.status || 'unknown';
    var steps = result.steps || 0;
    var mode = result.mode || 'mirage';

    if (status === 'ok') {
      logLines.push('');
      logLines.push('✅ 完了 (' + steps + 'ステップ' + (mode === 'adb_fallback' ? ', ADB直接' : '') + ')');
      showRunLog(logLines, 'success');
    } else if (status === 'cancelled') {
      logLines.push('');
      logLines.push('⏹ キャンセル (' + steps + 'ステップ実行済み)');
      showRunLog(logLines, 'cancelled');
    } else {
      logLines.push('');
      logLines.push('❌ エラー: ' + (result.error || 'unknown'));
      showRunLog(logLines, 'error');
    }
  } catch(e) {
    isMacroRunning = false;
    runBtn.style.display = 'inline-block';
    stopBtn.style.display = 'none';
    showRunLog(['❌ 実行失敗: ' + e], 'error');
  }
}

async function stopMacro() {
  if (!isMacroRunning) return;
  try {
    await window.pywebview.api.cancel_macro();
  } catch(e) {
    console.log('Cancel error:', e);
  }
}

function showRunLog(lines, status) {
  var codePreview = document.getElementById('code-output');
  var statusColor = {
    running: '#89b4fa',
    success: '#a6e3a1',
    cancelled: '#fab387',
    error: '#f38ba8'
  }[status] || '#cdd6f4';

  var header = document.getElementById('code-preview').querySelector('h3');
  if (status === 'running') {
    header.textContent = '⏳ 実行中...';
  } else {
    header.textContent = '実行ログ';
  }
  header.style.color = statusColor;

  codePreview.textContent = lines.join('\n');
  codePreview.style.color = statusColor;

  if (status !== 'running') {
    setTimeout(function() {
      header.textContent = '生成コード';
      header.style.color = '#89b4fa';
      codePreview.style.color = '#a6e3a1';
      updateCodePreview();
    }, 5000);
  }
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

  // Add stop button (hidden by default)
  var stopBtn = document.createElement('button');
  stopBtn.id = 'btn-stop';
  stopBtn.textContent = '⏹ 停止';
  stopBtn.onclick = stopMacro;
  stopBtn.style.cssText = 'background:#fab387;color:#1e1e2e;font-weight:bold;display:none;';
  document.getElementById('toolbar-buttons').insertBefore(
    stopBtn, document.getElementById('btn-save')
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
  currentPickerMeta = {
    serial: serial,
    basis_w: imgData.width || imgData.preview_w || imgData.native_w || 0,
    basis_h: imgData.height || imgData.preview_h || imgData.native_h || 0,
    preview_w: imgData.preview_w || imgData.width || 0,
    preview_h: imgData.preview_h || imgData.height || 0,
    native_w: imgData.native_w || 0,
    native_h: imgData.native_h || 0,
    coord_space: imgData.coord_space || 'preview'
  };
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
var currentPickerMeta = null;

function handlePickerClick(x, y, action, event) {
  if (action === 'tap') {
    recordedActions.push({type: 'tap', x: x, y: y, meta: currentPickerMeta});
    showClickMarker(event.clientX, event.clientY, 'TAP');
  } else if (action === 'long_press') {
    recordedActions.push({type: 'long_press', x: x, y: y, duration: 1000, meta: currentPickerMeta});
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
        x2: x, y2: y, duration: 300, meta: currentPickerMeta
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
async function createContainerFromRecording() {
  var name = prompt('コンテナ名を入力:', '録画_' + new Date().toLocaleTimeString());
  if (!name) name = '録画結果';

  var containerBlock = workspace.newBlock('adb_container');
  containerBlock.setFieldValue(name, 'NAME');
  containerBlock.initSvg();

  var prevBlock = null;
  for (const action of recordedActions) {
    var block = null;
    if (action.type === 'tap') {
      block = workspace.newBlock('adb_tap');
      block.setFieldValue(action.x, 'X');
      block.setFieldValue(action.y, 'Y');
      if (action.meta && action.meta.serial && action.meta.basis_w > 0 && action.meta.basis_h > 0) {
        try {
          var norm = await window.pywebview.api.normalize_coords(action.meta.serial, action.x, action.y, action.meta.basis_w, action.meta.basis_h);
          if (norm && norm.status === 'ok') {
            block.data = JSON.stringify({
              coord_basis: 'native_normalized',
              x_norm: norm.x_norm,
              y_norm: norm.y_norm
            });
          }
        } catch (e) { console.log('normalize tap failed', e); }
      }
    } else if (action.type === 'swipe') {
      block = workspace.newBlock('adb_swipe');
      block.setFieldValue(action.x1, 'X1');
      block.setFieldValue(action.y1, 'Y1');
      block.setFieldValue(action.x2, 'X2');
      block.setFieldValue(action.y2, 'Y2');
      block.setFieldValue(action.duration, 'DURATION');
      if (action.meta && action.meta.serial && action.meta.basis_w > 0 && action.meta.basis_h > 0) {
        try {
          var n1 = await window.pywebview.api.normalize_coords(action.meta.serial, action.x1, action.y1, action.meta.basis_w, action.meta.basis_h);
          var n2 = await window.pywebview.api.normalize_coords(action.meta.serial, action.x2, action.y2, action.meta.basis_w, action.meta.basis_h);
          if (n1 && n1.status === 'ok' && n2 && n2.status === 'ok') {
            block.data = JSON.stringify({
              coord_basis: 'native_normalized',
              x1_norm: n1.x_norm,
              y1_norm: n1.y_norm,
              x2_norm: n2.x_norm,
              y2_norm: n2.y_norm
            });
          }
        } catch (e) { console.log('normalize swipe failed', e); }
      }
    } else if (action.type === 'long_press') {
      block = workspace.newBlock('adb_long_press');
      block.setFieldValue(action.x, 'X');
      block.setFieldValue(action.y, 'Y');
      block.setFieldValue(action.duration, 'DURATION');
      if (action.meta && action.meta.serial && action.meta.basis_w > 0 && action.meta.basis_h > 0) {
        try {
          var normlp = await window.pywebview.api.normalize_coords(action.meta.serial, action.x, action.y, action.meta.basis_w, action.meta.basis_h);
          if (normlp && normlp.status === 'ok') {
            block.data = JSON.stringify({
              coord_basis: 'native_normalized',
              x_norm: normlp.x_norm,
              y_norm: normlp.y_norm
            });
          }
        } catch (e) { console.log('normalize long_press failed', e); }
      }
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
  }

  containerBlock.moveTo(new Blockly.utils.Coordinate(50, 50));
  containerBlock.render();
  containerBlock.setCollapsed(true);
  workspace.render();
  refreshNormalizationIndicators();
  recordedActions = [];
}

// ==================== Save / Load / Export ====================

async function normalizeCurrentBlocks(options) {
  options = options || {};
  const silent = !!options.silent;
  var serial = document.getElementById('device-select') ? document.getElementById('device-select').value : '';
  if (!serial) { if (!silent) alert('デバイスを選択してください'); return 0; }

  let imgData = null;
  try {
    imgData = await window.pywebview.api.capture_screen(serial);
  } catch (e) {
    if (!silent) alert('画面基準の取得に失敗: ' + e);
    return 0;
  }
  if (!imgData || imgData.error) {
    if (!silent) alert('画面基準の取得に失敗: ' + (imgData ? imgData.error : 'unknown'));
    return 0;
  }

  const basis_w = imgData.width || imgData.preview_w || imgData.native_w || 0;
  const basis_h = imgData.height || imgData.preview_h || imgData.native_h || 0;
  if (!(basis_w > 0 && basis_h > 0)) {
    if (!silent) alert('有効な座標基準がありません');
    return 0;
  }

  let touched = 0;
  const blocks = workspace.getAllBlocks(false);
  for (const block of blocks) {
    try {
      if (block.type === 'adb_tap') {
        const x = Number(block.getFieldValue('X'));
        const y = Number(block.getFieldValue('Y'));
        const norm = await window.pywebview.api.normalize_coords(serial, x, y, basis_w, basis_h);
        if (norm && norm.status === 'ok') {
          block.data = JSON.stringify({ coord_basis: 'native_normalized', x_norm: norm.x_norm, y_norm: norm.y_norm });
          touched++;
        }
      } else if (block.type === 'adb_swipe') {
        const x1 = Number(block.getFieldValue('X1'));
        const y1 = Number(block.getFieldValue('Y1'));
        const x2 = Number(block.getFieldValue('X2'));
        const y2 = Number(block.getFieldValue('Y2'));
        const n1 = await window.pywebview.api.normalize_coords(serial, x1, y1, basis_w, basis_h);
        const n2 = await window.pywebview.api.normalize_coords(serial, x2, y2, basis_w, basis_h);
        if (n1 && n1.status === 'ok' && n2 && n2.status === 'ok') {
          block.data = JSON.stringify({
            coord_basis: 'native_normalized',
            x1_norm: n1.x_norm, y1_norm: n1.y_norm,
            x2_norm: n2.x_norm, y2_norm: n2.y_norm
          });
          touched++;
        }
      } else if (block.type === 'adb_long_press') {
        const x = Number(block.getFieldValue('X'));
        const y = Number(block.getFieldValue('Y'));
        const norm = await window.pywebview.api.normalize_coords(serial, x, y, basis_w, basis_h);
        if (norm && norm.status === 'ok') {
          block.data = JSON.stringify({ coord_basis: 'native_normalized', x_norm: norm.x_norm, y_norm: norm.y_norm });
          touched++;
        }
      }
    } catch (e) {
      console.log('normalize block failed', block.type, e);
    }
  }

  updateCodePreview();
  refreshNormalizationIndicators();
  if (!silent) alert('正規化したブロック数: ' + touched);
  return touched;
}

async function saveMacro() {
  await normalizeCurrentBlocks({silent: true});
  var name = prompt('繝槭け繝ｭ蜷阪ｒ蛟､蜉', 'my_macro');
  if (!name) return;
  var ws = Blockly.serialization.workspaces.save(workspace);
  var code = Blockly.Python.workspaceToCode(workspace);
  var serial = document.getElementById('device-select') ? document.getElementById('device-select').value : '';
  var payload = {
    schema_version: 2,
    coord_policy: 'native_normalized',
    editor_basis: 'preview_preferred',
    saved_at: new Date().toISOString(),
    selected_device: serial || null,
    workspace: ws
  };
  var r = await window.pywebview.api.save_macro(name, payload, code);
  alert('菫晄戟蜿ｯ逶ｴ ' + r.path);
}

async function loadMacro() {
  var macros = await window.pywebview.api.list_macros();
  if (!macros || macros.length === 0) { alert('菫晄戟蛟､蜉縺後≠繧翫∪縺帙ｓ); return; }
  var name = prompt('隱ｯ縺ｿ霎ｼ縺ｿ繝槭け繝ｭ:
' + macros.join('
'));
  if (!name) return;
  var data = await window.pywebview.api.load_macro(name);
  if (!data) return;
  var ws = data.workspace ? data.workspace : data;
  Blockly.serialization.workspaces.load(ws, workspace);
  refreshNormalizationIndicators();
}

async function exportCode() {
  await normalizeCurrentBlocks({silent: true});
  var code = Blockly.Python.workspaceToCode(workspace);
  var blob = new Blob([code], {type: 'text/plain'});
  var a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'macro.py';
  a.click();
}
