// 1. Three.js 3D Vehicle Setup
let scene, camera, renderer, carMesh, cabinMesh, frontWheels = [];
let dirLight, ambientLight;

function init3D() {
  const container = document.getElementById('car3d-container');
  scene = new THREE.Scene();
  camera = new THREE.PerspectiveCamera(45, container.clientWidth / container.clientHeight, 0.1, 100);
  camera.position.set(3.2, 2.6, 4.2);
  camera.lookAt(0, 0, 0);

  renderer = new THREE.WebGLRenderer({ antialias: true, alpha: true });
  renderer.setSize(container.clientWidth, container.clientHeight);
  container.appendChild(renderer.domElement);

  // Bright multi-angle lighting for high contrast
  dirLight = new THREE.DirectionalLight(0xffffff, 2.8);
  dirLight.position.set(6, 12, 8);
  scene.add(dirLight);

  const fillLight = new THREE.DirectionalLight(0x38bdf8, 1.8);
  fillLight.position.set(-6, 8, -6);
  scene.add(fillLight);

  ambientLight = new THREE.AmbientLight(0x334155, 1.4);
  scene.add(ambientLight);

  // Car Body - enlarged scale (1.4x)
  const bodyGeo = new THREE.BoxGeometry(2.6, 0.65, 1.3);
  const bodyMat = new THREE.MeshStandardMaterial({
    color: 0x0284c7,
    metalness: 0.6,
    roughness: 0.2,
    emissive: 0x012b47,
    emissiveIntensity: 0.4
  });
  carMesh = new THREE.Mesh(bodyGeo, bodyMat);
  scene.add(carMesh);

  // Cabin
  const cabinGeo = new THREE.BoxGeometry(1.3, 0.5, 1.05);
  const cabinMat = new THREE.MeshStandardMaterial({
    color: 0x0f172a,
    roughness: 0.1,
    metalness: 0.8
  });
  cabinMesh = new THREE.Mesh(cabinGeo, cabinMat);
  cabinMesh.position.set(-0.25, 0.55, 0);
  carMesh.add(cabinMesh);

  // Wheels
  const wheelGeo = new THREE.CylinderGeometry(0.26, 0.26, 0.2, 20);
  const wheelMat = new THREE.MeshStandardMaterial({ color: 0x1e293b, roughness: 0.8 });
  wheelGeo.rotateX(Math.PI / 2);

  const positions = [
    [0.9, -0.25, 0.72],   // Front Right
    [0.9, -0.25, -0.72],  // Front Left
    [-0.9, -0.25, 0.72],  // Rear Right
    [-0.9, -0.25, -0.72]  // Rear Left
  ];

  positions.forEach((pos, idx) => {
    const wheel = new THREE.Mesh(wheelGeo, wheelMat);
    wheel.position.set(...pos);
    carMesh.add(wheel);
    if (idx < 2) frontWheels.push(wheel);
  });

  // Grid ground
  const grid = new THREE.GridHelper(24, 24, 0x38bdf8, 0x1e293b);
  grid.position.y = -0.52;
  scene.add(grid);

  function animate() {
    requestAnimationFrame(animate);
    grid.position.x = (grid.position.x - 0.05) % 1;
    renderer.render(scene, camera);
  }
  animate();
}
init3D();

// 2. Blackbox Replay State & Telemetry Polling Loop
let isReplayMode = false;
let replayIndex = -1;
let cachedHistory = [];
let lastViolCode = 0;

function setLiveMode() {
  isReplayMode = false;
  document.getElementById('btnLive').className = 'mode-btn active';
  document.getElementById('btnReplay').className = 'mode-btn';
  document.getElementById('renderModeLabel').innerText = 'LIVE TRACKING';
  document.getElementById('renderModeLabel').style.color = 'var(--text-muted)';
  const slider = document.getElementById('replaySlider');
  slider.value = cachedHistory.length ? cachedHistory.length - 1 : 299;
  document.getElementById('replayTimeLabel').innerText = '0.0s (Live Edge)';
}

function setReplayMode() {
  if (!cachedHistory.length) return;
  isReplayMode = true;
  document.getElementById('btnLive').className = 'mode-btn';
  document.getElementById('btnReplay').className = 'mode-btn replay-active';
  document.getElementById('renderModeLabel').innerText = 'REPLAY PAUSED';
  document.getElementById('renderModeLabel').style.color = 'var(--neon-amber)';
  if (replayIndex < 0) {
    replayIndex = cachedHistory.length - 1;
  }
  onScrubberInput(replayIndex);
}

function onScrubberInput(val) {
  if (!cachedHistory.length) return;
  isReplayMode = true;
  document.getElementById('btnLive').className = 'mode-btn';
  document.getElementById('btnReplay').className = 'mode-btn replay-active';
  document.getElementById('renderModeLabel').innerText = 'REPLAY SCRUB';
  document.getElementById('renderModeLabel').style.color = 'var(--neon-amber)';

  replayIndex = Math.min(Math.max(0, parseInt(val)), cachedHistory.length - 1);
  const frame = cachedHistory[replayIndex];
  const liveT = cachedHistory[cachedHistory.length - 1].t || 0;
  const frameT = frame.t || 0;
  const offsetSeconds = Math.max(0, liveT - frameT).toFixed(2);
  document.getElementById('replayTimeLabel').innerText = `-${offsetSeconds}s (Frame ${replayIndex}/${cachedHistory.length})`;

  renderFrame(frame);
}

function stepFrame(delta) {
  if (!cachedHistory.length) return;
  if (!isReplayMode) setReplayMode();
  const slider = document.getElementById('replaySlider');
  let target = (replayIndex >= 0 ? replayIndex : cachedHistory.length - 1) + delta;
  target = Math.min(Math.max(0, target), cachedHistory.length - 1);
  slider.value = target;
  onScrubberInput(target);
}

function renderFrame(frame) {
  if (!frame) return;
  document.getElementById('steerVal').innerText = (frame.steering !== undefined ? frame.steering : 0).toFixed(1) + '°';
  document.getElementById('speedVal').innerText = (frame.speed !== undefined ? frame.speed : 0).toFixed(0) + ' km/h';
  document.getElementById('throttleVal').innerText = (frame.throttle !== undefined ? frame.throttle : 0).toFixed(0) + ' %';

  // Ring, score, label and car color follow the guest FSM only; a violation code alone is not an emergency
  const isEmergency = (frame.fsm === 'EMERGENCY_STOP');
  const isDegraded = (frame.fsm === 'DEGRADED') || (frame.fsm === 'LIMP_HOME');
  const isNormal = (frame.fsm === 'NORMAL');

  // Read score with full compatibility for "score" or "safety_score"
  const rawScore = (frame.score !== undefined && frame.score !== null) ? frame.score : frame.safety_score;
  const isKnown = (rawScore !== undefined && rawScore !== null) || (frame.fsm && frame.fsm !== '알 수 없음 (UNKNOWN)');

  let score = null;
  if (!isKnown) {
    score = null;
  } else if (isEmergency) {
    score = 0.0;
  } else if (isDegraded) {
    score = (rawScore !== undefined && rawScore !== null && rawScore < 90) ? rawScore : 60.0;
  } else if (isNormal) {
    score = (rawScore !== undefined && rawScore !== null) ? rawScore : 100.0;
  } else {
    score = rawScore;
  }

  const scoreValEl = document.getElementById('scoreVal');
  const scoreLabel = document.getElementById('scoreLabel');
  const ring = document.getElementById('scoreRing');

  if (score === null || score === undefined) {
    scoreValEl.innerText = '알 수 없음';
    scoreValEl.style.fontSize = '1.35rem';
    scoreValEl.style.color = 'var(--text-muted)';
    ring.style.stroke = 'rgba(148, 163, 184, 0.4)';
    ring.style.strokeDashoffset = 264;
    scoreLabel.innerText = 'UNKNOWN';
    scoreLabel.style.color = 'var(--text-muted)';
  } else {
    scoreValEl.innerText = Math.round(score);
    scoreValEl.style.fontSize = '3.0rem';
    scoreValEl.style.color = 'var(--text)';
    const offset = frame.fsm === 'EMERGENCY_STOP' ? 0 : 264 - (264 * (Math.max(0, Math.min(100, score)) / 100));
    ring.style.strokeDashoffset = offset;

    if (isEmergency) {
      ring.style.stroke = 'var(--neon-red)';
      scoreLabel.innerText = 'EMERGENCY STOP';
      scoreLabel.style.color = 'var(--neon-red)';
    } else if (isDegraded) {
      ring.style.stroke = 'var(--neon-amber)';
      scoreLabel.innerText = frame.fsm;
      scoreLabel.style.color = 'var(--neon-amber)';
    } else if (isNormal && score >= 90) {
      ring.style.stroke = 'var(--neon-green)';
      scoreLabel.innerText = 'SAFE (NORMAL)';
      scoreLabel.style.color = 'var(--neon-green)';
    } else {
      ring.style.stroke = 'var(--neon-amber)';
      scoreLabel.innerText = frame.fsm ? frame.fsm : 'WARNING';
      scoreLabel.style.color = 'var(--neon-amber)';
    }
  }

  // Rule 2 & 4: 3D Vehicle Color Synchronizes with Safe State FSM
  if (carMesh && carMesh.material) {
    if (isEmergency) {
      carMesh.material.color.setHex(0xff2222); // Vivid Fluorescent Red
      carMesh.material.emissive.setHex(0xff0000); // Glowing Red Emissive
      carMesh.material.emissiveIntensity = 1.0;
      if (cabinMesh && cabinMesh.material) {
        cabinMesh.material.color.setHex(0x770000);
        cabinMesh.material.emissive.setHex(0x330000);
        cabinMesh.material.emissiveIntensity = 0.5;
      }
    } else if (isDegraded) {
      carMesh.material.color.setHex(0xf59e0b); // Amber Warning
      carMesh.material.emissive.setHex(0xd97706);
      carMesh.material.emissiveIntensity = 0.8;
      if (cabinMesh && cabinMesh.material) {
        cabinMesh.material.color.setHex(0x78350f);
        cabinMesh.material.emissive.setHex(0x451a03);
        cabinMesh.material.emissiveIntensity = 0.4;
      }
    } else {
      carMesh.material.color.setHex(0x38bdf8); // Vivid Light Sky Blue / Cyan
      carMesh.material.emissive.setHex(0x0284c7);
      carMesh.material.emissiveIntensity = 0.65;
      if (cabinMesh && cabinMesh.material) {
        cabinMesh.material.color.setHex(0x0f172a);
        cabinMesh.material.emissive.setHex(0x000000);
        cabinMesh.material.emissiveIntensity = 0.0;
      }
    }
  }

  const fsmBadge = document.getElementById('fsmBadge');
  if (fsmBadge && frame.fsm) {
    fsmBadge.innerText = `FSM: ${frame.fsm}`;
    const badgeCol = isEmergency ? 'var(--neon-red)' : (frame.fsm_color || 'var(--neon-green)');
    fsmBadge.style.borderColor = badgeCol;
    fsmBadge.style.color = badgeCol;
    fsmBadge.style.background = isEmergency ? 'rgba(239,68,68,0.25)' : `${frame.fsm_color || '#10b981'}25`;
  }

  const violEl = document.getElementById('violCode');
  if (violEl) {
    if (frame.viol && frame.viol !== 'NONE' && frame.viol !== '0x0 (NONE)' && frame.viol !== 'NORMAL') {
      violEl.innerText = frame.viol;
      violEl.style.color = 'var(--neon-red)';
    } else {
      violEl.innerText = 'NONE';
      violEl.style.color = 'var(--neon-green)';
    }
  }

  // Update 3D steering
  const steerRad = ((frame.steering || 0) * Math.PI) / 180;
  frontWheels.forEach(w => w.rotation.y = -steerRad * 0.4);
  carMesh.rotation.y = -steerRad * 0.15;
}

async function updateTelemetry() {
  try {
    const res = await fetch('/api/telemetry');
    const data = await res.json();

    cachedHistory = data.history || [];
    const slider = document.getElementById('replaySlider');
    slider.max = Math.max(0, cachedHistory.length - 1);

    // Connection badge
    const connText = document.getElementById('connText');
    const connDot = document.getElementById('connDot');
    const connStatus = document.getElementById('connStatus');

    if (data.status === 'CONNECTED') {
      connText.innerText = 'QNX 8.0 RTOS LIVE LINK';
      connDot.style.background = '#10b981';
      connStatus.style.borderColor = '#10b981';
    } else {
      connText.innerText = 'Waiting for QNX Node...';
      connDot.style.background = '#f59e0b';
      connStatus.style.borderColor = '#f59e0b';
      const fsmBadge = document.getElementById('fsmBadge');
      if (fsmBadge && !isReplayMode) {
        fsmBadge.innerText = 'FSM: 알 수 없음 (UNKNOWN)';
        fsmBadge.style.borderColor = '#94a3b8';
        fsmBadge.style.color = '#94a3b8';
        fsmBadge.style.background = 'rgba(148, 163, 184, 0.2)';
      }
      const schedBadge = document.getElementById('schedBadge');
      if (schedBadge) {
        schedBadge.innerText = '알 수 없음 (UNKNOWN)';
      }
      const violEl = document.getElementById('violCode');
      if (violEl && (!data.status || data.status !== 'CONNECTED')) {
        violEl.innerText = '알 수 없음';
        violEl.style.color = '#94a3b8';
      }
    }

    document.getElementById('packetCount').innerText = (data.total_packets || 0).toLocaleString();
    const schedBadge = document.getElementById('schedBadge');
    if (schedBadge && data.sched_policy) {
      schedBadge.innerText = data.sched_policy;
    }

    // Incident Logging
    if (data.violation_code !== 0 && data.violation_code !== lastViolCode) {
      addLog(`[VIOLATION TRAP] ${data.violation_name} (Code: 0x${data.violation_code.toString(16).toUpperCase()})`, true);
      lastViolCode = data.violation_code;
    } else if (data.violation_code === 0) {
      lastViolCode = 0;
    }

    // In LIVE mode, render real-time state
    if (!isReplayMode) {
      slider.value = cachedHistory.length ? cachedHistory.length - 1 : 0;
      document.getElementById('velVal').innerText = (data.steering_vel || 0).toFixed(1) + ' °/s';
      renderFrame({
        steering: data.steering,
        speed: data.speed,
        throttle: data.throttle,
        score: (data.score !== undefined && data.score !== null) ? data.score : data.safety_score,
        fsm: data.fsm_state,
        fsm_color: data.fsm_color,
        viol: data.violation_name
      });
    }

    // Always redraw Waveform with entire history
    drawWaveform(cachedHistory, isReplayMode ? replayIndex : -1);
  } catch (err) {
    console.error(err);
    const fsmBadge = document.getElementById('fsmBadge');
    if (fsmBadge && !isReplayMode) {
      fsmBadge.innerText = 'FSM: 알 수 없음 (UNKNOWN)';
      fsmBadge.style.borderColor = '#94a3b8';
      fsmBadge.style.color = '#94a3b8';
      fsmBadge.style.background = 'rgba(148, 163, 184, 0.2)';
    }
    const schedBadge = document.getElementById('schedBadge');
    if (schedBadge) {
      schedBadge.innerText = '알 수 없음 (UNKNOWN)';
    }
  }
}
setInterval(updateTelemetry, 100);

// 3. Canvas Waveform with Scrubber Marker
function drawWaveform(history, highlightIdx) {
  const canvas = document.getElementById('chart');
  const ctx = canvas.getContext('2d');
  const w = canvas.width;
  const h = canvas.height;
  ctx.clearRect(0, 0, w, h);

  if (!history.length) return;
  const maxPts = Math.max(history.length, 60);

  // Draw Grid
  ctx.strokeStyle = 'rgba(255, 255, 255, 0.05)';
  ctx.lineWidth = 1;
  for (let y = 30; y < h; y += 40) {
    ctx.beginPath();
    ctx.moveTo(0, y);
    ctx.lineTo(w, y);
    ctx.stroke();
  }

  // Draw Steering Line (Cyan)
  ctx.strokeStyle = '#38bdf8';
  ctx.lineWidth = 2;
  ctx.beginPath();
  history.forEach((pt, i) => {
    const x = (i / maxPts) * w;
    const y = h / 2 - ((pt.steering || 0) / 100) * (h / 2);
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });
  ctx.stroke();

  // Draw Speed Line (Emerald)
  ctx.strokeStyle = '#10b981';
  ctx.lineWidth = 1.5;
  ctx.beginPath();
  history.forEach((pt, i) => {
    const x = (i / maxPts) * w;
    const y = h - ((pt.speed || 0) / 120) * h;
    if (i === 0) ctx.moveTo(x, y);
    else ctx.lineTo(x, y);
  });
  ctx.stroke();

  // Draw Replay Scrubber Line (Amber)
  if (highlightIdx >= 0 && highlightIdx < history.length) {
    const scrubX = (highlightIdx / maxPts) * w;
    ctx.strokeStyle = '#f59e0b';
    ctx.lineWidth = 2;
    ctx.setLineDash([4, 4]);
    ctx.beginPath();
    ctx.moveTo(scrubX, 0);
    ctx.lineTo(scrubX, h);
    ctx.stroke();
    ctx.setLineDash([]);

    // Scrubber tag
    ctx.fillStyle = '#f59e0b';
    ctx.font = '10px monospace';
    ctx.fillText(`SCRUBBER [${highlightIdx}]`, Math.min(scrubX + 4, w - 90), 14);
  }
}

let realReplayState = 'idle';
async function refreshRealReplay() {
  try {
    const response = await fetch('/api/replay');
    if (!response.ok) throw new Error('Replay status unavailable');
    const data = await response.json();
    realReplayState = data.state;
    const active = ['starting', 'running', 'stopping'].includes(data.state);
    const button = document.getElementById('btnRealData');
    button.disabled = data.state === 'starting' || data.state === 'stopping';
    button.textContent = active ? '■ STOP REAL CAN' : '⚡ REAL CAN REPLAY';
    button.classList.toggle('active', active);
    const details = data.packets ? ` (${data.packets} packets, max gap ${data.max_gap_ms} ms)` : '';
    document.getElementById('realReplayStatus').textContent = (data.message || 'Real CAN replay ready (10 seconds)') + details;
    if (active && !isReplayMode) {
      document.getElementById('renderModeLabel').innerText = 'REAL VEHICLE CAN REPLAY (comma2k19)';
    } else if (!active && !isReplayMode && document.getElementById('renderModeLabel').innerText.startsWith('REAL VEHICLE')) {
      document.getElementById('renderModeLabel').innerText = 'LIVE TRACKING';
    }
  } catch (error) {
    document.getElementById('realReplayStatus').textContent = `Replay status unavailable: ${error.message}`;
  }
}
async function setRealDataMode() {
  const button = document.getElementById('btnRealData');
  button.disabled = true;
  try {
    const action = realReplayState === 'running' ? 'stop' : 'start';
    const response = await fetch(`/api/replay/${action}`, { method: 'POST' });
    const data = await response.json();
    if (!response.ok || !data.success) throw new Error(data.message);
    if (action === 'start') setLiveMode();
    await refreshRealReplay();
  } catch (error) {
    document.getElementById('realReplayStatus').textContent = `Replay failed: ${error.message}`;
    button.disabled = false;
  }
}
setInterval(refreshRealReplay, 500);
refreshRealReplay();

async function runRogueAttack() {
  const button = document.getElementById('rogueAttackButton');
  const result = document.getElementById('rogueAttackResult');
  button.disabled = true;
  result.textContent = 'Running guest security check...';
  try {
    const response = await fetch('/api/rogue-attack', { method: 'POST' });
    const body = await response.json();
    if (!body.success) throw new Error(body.message);
    const r = body.result;
    if (r.launch !== 0) throw new Error(`Guest could not run the attack (errno ${r.launch}).`);
    const rejected = n => n === 1 || n === 13;
    const open = rejected(r.open_write)
      ? `Write open refused by SafePulse resource manager using client credentials from QNX (errno ${r.open_write}).`
      : `Write open result: errno ${r.open_write}; write result: ${r.write < 0 ? 'not attempted' : `errno ${r.write}`}.`;
    const reset = rejected(r.reset)
      ? `Reset refused by SafePulse resource manager using client credentials from QNX (errno ${r.reset}).`
      : `Reset open: errno ${r.open_reset}; reset: ${r.reset < 0 ? 'not attempted' : `errno ${r.reset}`}.`;
    const signal = rejected(r.signal)
      ? `SIGKILL refused by QNX process manager / kernel ability check (errno ${r.signal}).`
      : `SIGKILL result: errno ${r.signal}.`;
    result.textContent = `${open} ${reset} ${signal}`;
    addLog(`[ROGUE] ${result.textContent}`, true);
  } catch (error) {
    result.textContent = `Attack result unavailable: ${error.message}`;
  } finally {
    button.disabled = false;
  }
}

// 4. Send Chaos Command
async function sendChaos(type) {
  try {
    const res = await fetch(`/api/chaos/${type}`, { method: 'POST' });
    const resData = await res.json();
    addLog(`[CHAOS DISPATCH] Fault Type ${type}: ${resData.message}`, true);
    if (type === 0) {
      lastViolCode = 0;
      const violEl = document.getElementById('violCode');
      if (violEl) {
        violEl.innerText = 'NONE';
        violEl.style.color = 'var(--neon-green)';
      }
    }
  } catch (e) {
    addLog(`[ERROR] Chaos dispatch failed: ${e}`, true);
  }
}

function addLog(text, isViol = false) {
  const list = document.getElementById('logList');
  const item = document.createElement('div');
  item.className = 'log-item' + (isViol ? ' viol' : '');
  const timeStr = new Date().toLocaleTimeString();
  item.innerHTML = `<span>${text}</span><span style="color: var(--text-muted);">${timeStr}</span>`;
  list.prepend(item);
}
